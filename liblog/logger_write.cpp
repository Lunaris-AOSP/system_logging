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

#define _POSIX_THREAD_SAFE_FUNCTIONS  // For mingw localtime_r().

#include "logger_write.h"

#include <errno.h>
#include <inttypes.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#ifdef __BIONIC__
#include <android/set_abort_message.h>
#endif

#include <atomic>

#include <android-base/errno_restorer.h>
#include <android-base/macros.h>
#include <private/android_filesystem_config.h>
#include <private/android_logger.h>

#include "android/log.h"
#include "log/log_read.h"
#include "logger.h"
#include "uio.h"

#ifdef __ANDROID__
#include "logd_writer.h"
#include "pmsg_writer.h"
#endif

#if defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <syscall.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

// The preferred way to access system properties is using android::base::GetProperty in libbase.
// However, adding dependency to libbase requires that if liblog was statically linked to a client,
// that client now has additional dependency to libbase as well because static dependencies of
// static library is not exported. (users of liblog.so however is fine).
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

using android::base::ErrnoRestorer;

#define LOG_BUF_SIZE 1024

#if defined(__ANDROID__)
static int check_log_uid_permissions() {
  uid_t uid = getuid();

  /* Matches clientCanWriteSecurityLog() in logd */
  if ((uid != AID_SYSTEM) && (uid != AID_ROOT) && (uid != AID_LOG)) {
    uid = geteuid();
    if ((uid != AID_SYSTEM) && (uid != AID_ROOT) && (uid != AID_LOG)) {
      gid_t gid = getgid();
      if ((gid != AID_SYSTEM) && (gid != AID_ROOT) && (gid != AID_LOG)) {
        gid = getegid();
        if ((gid != AID_SYSTEM) && (gid != AID_ROOT) && (gid != AID_LOG)) {
          int num_groups;
          gid_t* groups;

          num_groups = getgroups(0, NULL);
          if (num_groups <= 0) {
            return -EPERM;
          }
          groups = static_cast<gid_t*>(calloc(num_groups, sizeof(gid_t)));
          if (!groups) {
            return -ENOMEM;
          }
          num_groups = getgroups(num_groups, groups);
          while (num_groups > 0) {
            if (groups[num_groups - 1] == AID_LOG ||
                groups[num_groups - 1] == AID_SECURITY_LOG_WRITER) {
              break;
            }
            --num_groups;
          }
          free(groups);
          if (num_groups <= 0) {
            return -EPERM;
          }
        }
      }
    }
  }
  return 0;
}
#endif

/*
 * Release any logger resources. A new log write will immediately re-acquire.
 */
void __android_log_close() {
#ifdef __ANDROID__
  LogdClose();
  PmsgClose();
#endif
}

// BSD-based systems like Android/macOS have getprogname(). Others need us to provide one.
#if !defined(__APPLE__) && !defined(__BIONIC__)
static const char* getprogname() {
#ifdef _WIN32
  static bool first = true;
  static char progname[MAX_PATH] = {};

  if (first) {
    char path[PATH_MAX + 1];
    DWORD result = GetModuleFileName(nullptr, path, sizeof(path) - 1);
    if (result == 0 || result == sizeof(path) - 1) return "";
    path[PATH_MAX - 1] = 0;

    char* path_basename = basename(path);

    snprintf(progname, sizeof(progname), "%s", path_basename);
    first = false;
  }

  return progname;
#else
  return program_invocation_short_name;
#endif
}
#endif

// It's possible for logging to happen during static initialization before our globals are
// initialized, so we place this std::string in a function such that it is initialized on the first
// call. We use a pointer to avoid exit time destructors.
std::string& GetDefaultTag() {
  static std::string* default_tag = new std::string(getprogname());
  return *default_tag;
}

void __android_log_set_default_tag(const char* tag) {
  GetDefaultTag().assign(tag, 0, LOGGER_ENTRY_MAX_PAYLOAD);
}

static std::atomic_int32_t minimum_log_priority = ANDROID_LOG_DEFAULT;
int32_t __android_log_set_minimum_priority(int32_t priority) {
  return minimum_log_priority.exchange(priority, std::memory_order_relaxed);
}

int32_t __android_log_get_minimum_priority() {
  return minimum_log_priority;
}

#ifdef __ANDROID__
static __android_logger_function logger_function = __android_log_logd_logger;
#else
static __android_logger_function logger_function = __android_log_stderr_logger;
#endif

void __android_log_set_logger(__android_logger_function logger) {
  logger_function = logger;
}

void __android_log_default_aborter(const char* abort_message) {
#ifdef __ANDROID__
  android_set_abort_message(abort_message);
#else
  UNUSED(abort_message);
#endif
  abort();
}

static __android_aborter_function aborter_function = __android_log_default_aborter;

void __android_log_set_aborter(__android_aborter_function aborter) {
  aborter_function = aborter;
}

void __android_log_call_aborter(const char* abort_message) {
  aborter_function(abort_message);
}

#ifdef __ANDROID__
static int write_to_log(log_id_t log_id, struct iovec* vec, size_t nr,
                        const struct timespec* timestamp = nullptr) {
  if (log_id == LOG_ID_KERNEL) {
    return -EINVAL;
  }

  struct timespec ts;
  if (LIKELY(timestamp == nullptr)) {
    clock_gettime(CLOCK_REALTIME, &ts);
    timestamp = &ts;
  }

  if (log_id == LOG_ID_SECURITY) {
    if (vec[0].iov_len < 4) {
      return -EINVAL;
    }

    int ret = check_log_uid_permissions();
    if (ret < 0) {
      return ret;
    }
    if (!__android_log_security()) {
      /* If only we could reset downstream logd counter */
      return -EPERM;
    }
  } else if (log_id == LOG_ID_EVENTS || log_id == LOG_ID_STATS) {
    if (vec[0].iov_len < 4) {
      return -EINVAL;
    }
  }

  int ret = LogdWrite(log_id, timestamp, vec, nr);
  PmsgWrite(log_id, timestamp, vec, nr);

  return ret;
}
#else
static int write_to_log(log_id_t, struct iovec*, size_t, const struct timespec* = nullptr) {
  // Non-Android text logs should go to __android_log_stderr_logger, not here.
  // Non-Android binary logs are always dropped.
  return 1;
}
#endif

// Copied from base/threads.cpp
static uint64_t GetThreadId() {
#if defined(__BIONIC__)
  return gettid();
#elif defined(__APPLE__)
  uint64_t tid;
  pthread_threadid_np(NULL, &tid);
  return tid;
#elif defined(__linux__)
  return syscall(__NR_gettid);
#elif defined(_WIN32)
  return GetCurrentThreadId();
#endif
}

static void filestream_logger(const struct __android_log_message* log_message, FILE* stream) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);

  struct tm now;
  localtime_r(&ts.tv_sec, &now);

  char timestamp[sizeof("mm-DD HH::MM::SS.mmm\0")];
  size_t n = strftime(timestamp, sizeof(timestamp), "%m-%d %H:%M:%S", &now);
  snprintf(timestamp + n, sizeof(timestamp) - n, ".%03ld", ts.tv_nsec / (1000 * 1000));

  static const char log_characters[] = "XXVDIWEF";
  static_assert(arraysize(log_characters) - 1 == ANDROID_LOG_SILENT,
                "Mismatch in size of log_characters and values in android_LogPriority");
  int32_t priority =
      log_message->priority > ANDROID_LOG_SILENT ? ANDROID_LOG_FATAL : log_message->priority;
  char priority_char = log_characters[priority];
  uint64_t tid = GetThreadId();
  const char* tag = log_message->tag ? log_message->tag : " nullptr";

  if (log_message->file != nullptr) {
    fprintf(stream, "%s %5d %5" PRIu64 " %c %-8s: %s:%u %s\n", timestamp, getpid(), tid,
            priority_char, tag, log_message->file, log_message->line, log_message->message);
  } else {
    fprintf(stream, "%s %5d %5" PRIu64 " %c %-8s: %s\n", timestamp, getpid(), tid, priority_char,
            tag, log_message->message);
  }
}

static const char* get_file_logger_path() {
#ifdef __ANDROID__
  static const char* file_logger_path = []() {
    static char path[PROP_VALUE_MAX] = {};
    if (__system_property_get("ro.log.file_logger.path", path) > 0) {
      return path;
    }
    return static_cast<char*>(nullptr);  // means file_logger should not be used
  }();
  return file_logger_path;
#else
  return nullptr;
#endif
}

/*
 * If ro.log.file_logger.path is set to a file, send log_message to the file instead. This is for
 * Android-like environments where logd is not available; e.g. Microdroid. If the file is not
 * accessible (but ro.log.file_logger.path is set anyway), stderr is chosen as the fallback.
 *
 * Returns true if log was sent to file. false, if not.
 */
static bool log_to_file_if_overridden(const struct __android_log_message* log_message) {
  const char* file_logger_path = get_file_logger_path();
  if (file_logger_path == nullptr) return false;

  static FILE* stream = [&file_logger_path]() {
    FILE* f = fopen(file_logger_path, "ae");
    if (f != nullptr) return f;
    using namespace std::string_literals;
    std::string err_msg = "Cannot open "s + file_logger_path + " for logging: (" + strerror(errno) +
                          "). Falling back to stderr";
    __android_log_message m = {sizeof(__android_log_message),
                               LOG_ID_DEFAULT,
                               ANDROID_LOG_WARN,
                               "liblog",
                               __FILE__,
                               __LINE__,
                               err_msg.c_str()};
    filestream_logger(&m, stderr);
    return stderr;
  }();
  filestream_logger(log_message, stream);
  return true;
}

void __android_log_stderr_logger(const struct __android_log_message* log_message) {
  filestream_logger(log_message, stderr);
}

void __android_log_logd_logger(const struct __android_log_message* log_message) {
  __android_log_logd_logger_with_timestamp(log_message, nullptr);
}

void __android_log_logd_logger_with_timestamp(const struct __android_log_message* log_message,
                                              const struct timespec* timestamp) {
  if (log_to_file_if_overridden(log_message)) return;

  int buffer_id = log_message->buffer_id == LOG_ID_DEFAULT ? LOG_ID_MAIN : log_message->buffer_id;

  struct iovec vec[3];
  vec[0].iov_base =
      const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(&log_message->priority));
  vec[0].iov_len = 1;
  vec[1].iov_base = const_cast<void*>(static_cast<const void*>(log_message->tag));
  vec[1].iov_len = strlen(log_message->tag) + 1;
  vec[2].iov_base = const_cast<void*>(static_cast<const void*>(log_message->message));
  vec[2].iov_len = strlen(log_message->message) + 1;

  write_to_log(static_cast<log_id_t>(buffer_id), vec, 3, timestamp);
}

int __android_log_write(int prio, const char* tag, const char* msg) {
  return __android_log_buf_write(LOG_ID_MAIN, prio, tag, msg);
}

void __android_log_write_log_message(__android_log_message* log_message) {
  ErrnoRestorer errno_restorer;

  if (log_message->buffer_id != LOG_ID_DEFAULT && log_message->buffer_id != LOG_ID_MAIN &&
      log_message->buffer_id != LOG_ID_SYSTEM && log_message->buffer_id != LOG_ID_RADIO &&
      log_message->buffer_id != LOG_ID_CRASH) {
    return;
  }

  if (log_message->tag == nullptr) {
    log_message->tag = GetDefaultTag().c_str();
  }

#if __BIONIC__
  if (log_message->priority == ANDROID_LOG_FATAL) {
    android_set_abort_message(log_message->message);
  }
#endif

  logger_function(log_message);
}

int __android_log_buf_write(int log_id, int prio, const char* tag, const char* msg) {
  ErrnoRestorer errno_restorer;

  if (!__android_log_is_loggable(prio, tag, ANDROID_LOG_VERBOSE)) {
    return -EPERM;
  }

  __android_log_message log_message = {
      sizeof(__android_log_message), log_id, prio, tag, nullptr, 0, msg};
  __android_log_write_log_message(&log_message);
  return 1;
}

int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
  ErrnoRestorer errno_restorer;

  if (!__android_log_is_loggable(prio, tag, ANDROID_LOG_VERBOSE)) {
    return -EPERM;
  }

  __attribute__((uninitialized)) char buf[LOG_BUF_SIZE];

  vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);

  __android_log_message log_message = {
      sizeof(__android_log_message), LOG_ID_MAIN, prio, tag, nullptr, 0, buf};
  __android_log_write_log_message(&log_message);
  return 1;
}

int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
  ErrnoRestorer errno_restorer;

  if (!__android_log_is_loggable(prio, tag, ANDROID_LOG_VERBOSE)) {
    return -EPERM;
  }

  va_list ap;
  __attribute__((uninitialized)) char buf[LOG_BUF_SIZE];

  va_start(ap, fmt);
  vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
  va_end(ap);

  __android_log_message log_message = {
      sizeof(__android_log_message), LOG_ID_MAIN, prio, tag, nullptr, 0, buf};
  __android_log_write_log_message(&log_message);
  return 1;
}

int __android_log_buf_print(int log_id, int prio, const char* tag, const char* fmt, ...) {
  ErrnoRestorer errno_restorer;

  if (!__android_log_is_loggable(prio, tag, ANDROID_LOG_VERBOSE)) {
    return -EPERM;
  }

  va_list ap;
  __attribute__((uninitialized)) char buf[LOG_BUF_SIZE];

  va_start(ap, fmt);
  vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
  va_end(ap);

  __android_log_message log_message = {
      sizeof(__android_log_message), log_id, prio, tag, nullptr, 0, buf};
  __android_log_write_log_message(&log_message);
  return 1;
}

void __android_log_assert(const char* cond, const char* tag, const char* fmt, ...) {
  __attribute__((uninitialized)) char buf[LOG_BUF_SIZE];

  if (fmt) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, LOG_BUF_SIZE, fmt, ap);
    va_end(ap);
  } else {
    /* Msg not provided, log condition.  N.B. Do not use cond directly as
     * format string as it could contain spurious '%' syntax (e.g.
     * "%d" in "blocks%devs == 0").
     */
    if (cond)
      snprintf(buf, LOG_BUF_SIZE, "Assertion failed: %s", cond);
    else
      strcpy(buf, "Unspecified assertion failed");
  }

  // Log assertion failures to stderr for the benefit of "adb shell" users
  // and gtests (http://b/23675822).
  TEMP_FAILURE_RETRY(write(2, buf, strlen(buf)));
  TEMP_FAILURE_RETRY(write(2, "\n", 1));

  __android_log_write(ANDROID_LOG_FATAL, tag, buf);
  __android_log_call_aborter(buf);
  abort();
}

int __android_log_bwrite(int32_t tag, const void* payload, size_t len) {
  ErrnoRestorer errno_restorer;

  struct iovec vec[2];

  vec[0].iov_base = &tag;
  vec[0].iov_len = sizeof(tag);
  vec[1].iov_base = (void*)payload;
  vec[1].iov_len = len;

  return write_to_log(LOG_ID_EVENTS, vec, 2);
}

int __android_log_stats_bwrite(int32_t tag, const void* payload, size_t len) {
  ErrnoRestorer errno_restorer;

  struct iovec vec[2];

  vec[0].iov_base = &tag;
  vec[0].iov_len = sizeof(tag);
  vec[1].iov_base = (void*)payload;
  vec[1].iov_len = len;

  return write_to_log(LOG_ID_STATS, vec, 2);
}

int __android_log_security_bwrite(int32_t tag, const void* payload, size_t len) {
  ErrnoRestorer errno_restorer;

  struct iovec vec[2];

  vec[0].iov_base = &tag;
  vec[0].iov_len = sizeof(tag);
  vec[1].iov_base = (void*)payload;
  vec[1].iov_len = len;

  return write_to_log(LOG_ID_SECURITY, vec, 2);
}

/*
 * Like __android_log_bwrite, but takes the type as well.  Doesn't work
 * for the general case where we're generating lists of stuff, but very
 * handy if we just want to dump an integer into the log.
 */
int __android_log_btwrite(int32_t tag, char type, const void* payload, size_t len) {
  ErrnoRestorer errno_restorer;

  struct iovec vec[3];

  vec[0].iov_base = &tag;
  vec[0].iov_len = sizeof(tag);
  vec[1].iov_base = &type;
  vec[1].iov_len = sizeof(type);
  vec[2].iov_base = (void*)payload;
  vec[2].iov_len = len;

  return write_to_log(LOG_ID_EVENTS, vec, 3);
}

/*
 * Like __android_log_bwrite, but used for writing strings to the
 * event log.
 */
int __android_log_bswrite(int32_t tag, const char* payload) {
  ErrnoRestorer errno_restorer;

  struct iovec vec[4];
  char type = EVENT_TYPE_STRING;
  uint32_t len = strlen(payload);

  vec[0].iov_base = &tag;
  vec[0].iov_len = sizeof(tag);
  vec[1].iov_base = &type;
  vec[1].iov_len = sizeof(type);
  vec[2].iov_base = &len;
  vec[2].iov_len = sizeof(len);
  vec[3].iov_base = (void*)payload;
  vec[3].iov_len = len;

  return write_to_log(LOG_ID_EVENTS, vec, 4);
}

/*
 * Like __android_log_security_bwrite, but used for writing strings to the
 * security log.
 */
int __android_log_security_bswrite(int32_t tag, const char* payload) {
  ErrnoRestorer errno_restorer;

  struct iovec vec[4];
  char type = EVENT_TYPE_STRING;
  uint32_t len = strlen(payload);

  vec[0].iov_base = &tag;
  vec[0].iov_len = sizeof(tag);
  vec[1].iov_base = &type;
  vec[1].iov_len = sizeof(type);
  vec[2].iov_base = &len;
  vec[2].iov_len = sizeof(len);
  vec[3].iov_base = (void*)payload;
  vec[3].iov_len = len;

  return write_to_log(LOG_ID_SECURITY, vec, 4);
}
