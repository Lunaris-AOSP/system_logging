/*
 * Copyright (C) 2007-2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "logd_writer.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <private/android_filesystem_config.h>
#include <private/android_logger.h>

#include "logger.h"
#include "uio.h"

class LogdSocket {
 public:
  static LogdSocket& BlockingSocket() {
    static LogdSocket logd_socket(true);
    return logd_socket;
  }
  static LogdSocket& NonBlockingSocket() {
    static LogdSocket logd_socket(false);
    return logd_socket;
  }

  void Reconnect() { LogdConnect(sock_); }

  // Zygote uses this to clean up open FD's after fork() and before specialization.  It is single
  // threaded at this point and therefore this function is explicitly not thread safe.  It sets
  // sock_ to kUninitialized, so future logs will be safely initialized whenever they happen.
  void Close() {
    if (sock_ != kUninitialized) {
      close(sock_);
    }
    sock_ = kUninitialized;
  }

  int sock() {
    GetSocket();
    return sock_;
  }

 private:
  LogdSocket(bool blocking) : blocking_(blocking) {}

  // Note that it is safe to call connect() multiple times on DGRAM Unix domain sockets, so this
  // function is used to reconnect to logd without requiring a new socket.
  static void LogdConnect(int sock) {
    sockaddr_un un = {};
    un.sun_family = AF_UNIX;
    strcpy(un.sun_path, "/dev/socket/logdw");
    TEMP_FAILURE_RETRY(connect(sock, reinterpret_cast<sockaddr*>(&un), sizeof(sockaddr_un)));
  }

  // sock_ should only be opened once.  If we see that sock_ is uninitialized, we
  // create a new socket and attempt to exchange it into the atomic sock_.  If the
  // compare/exchange was successful, then that will be the socket used for the duration of the
  // program, otherwise a different thread has already opened and written the socket to the atomic,
  // so close the new socket and return.
  void GetSocket() {
    if (sock_ != kUninitialized) {
      return;
    }

    int flags = SOCK_DGRAM | SOCK_CLOEXEC;
    if (!blocking_) {
      flags |= SOCK_NONBLOCK;
    }
    int new_socket = TEMP_FAILURE_RETRY(socket(PF_UNIX, flags, 0));
    if (new_socket < 0) {
      return;
    }

    LogdConnect(new_socket);

    int uninitialized_value = kUninitialized;
    if (!sock_.compare_exchange_strong(uninitialized_value, new_socket)) {
      close(new_socket);
      return;
    }
  }

  static const int kUninitialized = -1;
  atomic_int sock_ = kUninitialized;
  bool blocking_;
};

void LogdClose() {
  LogdSocket::BlockingSocket().Close();
  LogdSocket::NonBlockingSocket().Close();
}

int LogdWrite(log_id_t logId, const struct timespec* ts, const struct iovec* vec, size_t nr) {
  ssize_t ret;
  static const unsigned headerLength = 1;
  struct iovec newVec[nr + headerLength];
  android_log_header_t header;
  size_t i, payloadSize;
  static atomic_int dropped;

  LogdSocket& logd_socket =
      logId == LOG_ID_SECURITY ? LogdSocket::BlockingSocket() : LogdSocket::NonBlockingSocket();

  if (logd_socket.sock() < 0) {
    return -EBADF;
  }

  /* logd, after initialization and priv drop */
  if (getuid() == AID_LOGD) {
    /*
     * ignore log messages we send to ourself (logd).
     * Such log messages are often generated by libraries we depend on
     * which use standard Android logging.
     */
    return 0;
  }

  header.tid = gettid();
  header.realtime.tv_sec = ts->tv_sec;
  header.realtime.tv_nsec = ts->tv_nsec;

  newVec[0].iov_base = (unsigned char*)&header;
  newVec[0].iov_len = sizeof(header);

  int32_t snapshot = atomic_exchange_explicit(&dropped, 0, memory_order_relaxed);
  if (snapshot && __android_log_is_loggable_len(ANDROID_LOG_INFO, "liblog", strlen("liblog"),
                                                ANDROID_LOG_VERBOSE)) {
    android_log_event_int_t buffer;

    header.id = LOG_ID_EVENTS;
    buffer.header.tag = LIBLOG_LOG_TAG;
    buffer.payload.type = EVENT_TYPE_INT;
    buffer.payload.data = snapshot;

    newVec[headerLength].iov_base = &buffer;
    newVec[headerLength].iov_len = sizeof(buffer);

    ret = TEMP_FAILURE_RETRY(writev(logd_socket.sock(), newVec, 2));
    if (ret != (ssize_t)(sizeof(header) + sizeof(buffer))) {
      atomic_fetch_add_explicit(&dropped, snapshot, memory_order_relaxed);
    }
  }

  header.id = logId;

  for (payloadSize = 0, i = headerLength; i < nr + headerLength; i++) {
    newVec[i].iov_base = vec[i - headerLength].iov_base;
    payloadSize += newVec[i].iov_len = vec[i - headerLength].iov_len;

    if (payloadSize > LOGGER_ENTRY_MAX_PAYLOAD) {
      newVec[i].iov_len -= payloadSize - LOGGER_ENTRY_MAX_PAYLOAD;
      if (newVec[i].iov_len) {
        ++i;
      }
      break;
    }
  }

  // EAGAIN occurs if logd is overloaded, other errors indicate that something went wrong with
  // the connection, so we reset it and try again.
  ret = TEMP_FAILURE_RETRY(writev(logd_socket.sock(), newVec, i));
  if (ret < 0 && errno != EAGAIN) {
    logd_socket.Reconnect();

    ret = TEMP_FAILURE_RETRY(writev(logd_socket.sock(), newVec, i));
  }

  if (ret < 0) {
    ret = -errno;
  }

  if (ret > (ssize_t)sizeof(header)) {
    ret -= sizeof(header);
  } else if (ret < 0) {
    atomic_fetch_add_explicit(&dropped, 1, memory_order_relaxed);
  }

  return ret;
}
