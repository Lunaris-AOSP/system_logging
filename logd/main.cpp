/*
 * Copyright (C) 2012-2013 The Android Open Source Project
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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/klog.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <memory>

#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <cutils/android_get_control_file.h>
#include <cutils/sockets.h>
#include <log/event_tag_map.h>
#include <private/android_filesystem_config.h>
#include <private/android_logger.h>
#include <processgroup/sched_policy.h>
#include <utils/threads.h>

#include "CommandListener.h"
#include "LogAudit.h"
#include "LogBuffer.h"
#include "LogKlog.h"
#include "LogListener.h"
#include "LogReader.h"
#include "LogStatistics.h"
#include "LogTags.h"
#include "LogUtils.h"
#include "SerializedLogBuffer.h"
#include "SimpleLogBuffer.h"
#include "TrustyLog.h"

using android::base::GetBoolProperty;
using android::base::GetProperty;
using android::base::SetProperty;

#define KMSG_PRIORITY(PRI)                                 \
    '<', '0' + LOG_MAKEPRI(LOG_DAEMON, LOG_PRI(PRI)) / 10, \
        '0' + LOG_MAKEPRI(LOG_DAEMON, LOG_PRI(PRI)) % 10, '>'

// The service is designed to be run by init, it does not respond well to starting up manually. Init
// has a 'sigstop' feature that sends SIGSTOP to a service immediately before calling exec().  This
// allows debuggers, etc to be attached to logd at the very beginning, while still having init
// handle the user, groups, capabilities, files, etc setup.
static void DropPrivs(bool klogd, bool auditd) {
    if (set_sched_policy(0, SP_BACKGROUND) < 0) {
        PLOG(FATAL) << "failed to set background scheduling policy";
    }

    if (!GetBoolProperty("ro.debuggable", false)) {
        if (prctl(PR_SET_DUMPABLE, 0) == -1) {
            PLOG(FATAL) << "failed to clear PR_SET_DUMPABLE";
        }
    }

    std::unique_ptr<struct _cap_struct, int (*)(void*)> caps(cap_init(), cap_free);
    if (cap_clear(caps.get()) < 0) {
        PLOG(FATAL) << "cap_clear() failed";
    }
    if (klogd) {
        cap_value_t cap_syslog = CAP_SYSLOG;
        if (cap_set_flag(caps.get(), CAP_PERMITTED, 1, &cap_syslog, CAP_SET) < 0 ||
            cap_set_flag(caps.get(), CAP_EFFECTIVE, 1, &cap_syslog, CAP_SET) < 0) {
            PLOG(FATAL) << "Failed to set CAP_SYSLOG";
        }
    }
    if (auditd) {
        cap_value_t cap_audit_control = CAP_AUDIT_CONTROL;
        if (cap_set_flag(caps.get(), CAP_PERMITTED, 1, &cap_audit_control, CAP_SET) < 0 ||
            cap_set_flag(caps.get(), CAP_EFFECTIVE, 1, &cap_audit_control, CAP_SET) < 0) {
            PLOG(FATAL) << "Failed to set CAP_AUDIT_CONTROL";
        }
    }
    if (cap_set_proc(caps.get()) < 0) {
        PLOG(FATAL) << "cap_set_proc() failed";
    }
}

static void readDmesg(LogAudit* al, LogKlog* kl) {
    if (!al && !kl) {
        return;
    }

    int rc = klogctl(KLOG_SIZE_BUFFER, nullptr, 0);
    if (rc <= 0) {
        return;
    }

    // Margin for additional input race or trailing nul
    ssize_t len = rc + 1024;
    std::unique_ptr<char[]> buf(new char[len]);

    // Drop old logs in /proc/kmsg to avoid duplicate print.
    rc = klogctl(KLOG_SIZE_UNREAD, nullptr, 0);
    if (rc > 0)
        rc = klogctl(KLOG_READ, buf.get(), rc);


    rc = klogctl(KLOG_READ_ALL, buf.get(), len);
    if (rc <= 0) {
        return;
    }

    if (rc < len) {
        len = rc + 1;
    }
    buf[--len] = '\0';

    ssize_t sublen;
    for (char *ptr = nullptr, *tok = buf.get();
         (rc >= 0) && !!(tok = android::log_strntok_r(tok, len, ptr, sublen));
         tok = nullptr) {
        if ((sublen <= 0) || !*tok) continue;
        if (al) {
            rc = al->log(tok, sublen);
        }
        if (kl) {
            rc = kl->log(tok, sublen);
        }
    }
}

static int issueReinit() {
    int sock = TEMP_FAILURE_RETRY(socket_local_client(
        "logd", ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_STREAM));
    if (sock < 0) return -errno;

    static const char reinitStr[] = "reinit";
    ssize_t ret = TEMP_FAILURE_RETRY(write(sock, reinitStr, sizeof(reinitStr)));
    if (ret < 0) return -errno;

    struct pollfd p = {.fd = sock, .events = POLLIN};
    ret = TEMP_FAILURE_RETRY(poll(&p, 1, 1000));
    if (ret < 0) return -errno;
    if ((ret == 0) || !(p.revents & POLLIN)) return -ETIME;

    static const char success[] = "success";
    char buffer[sizeof(success) - 1] = {};
    ret = TEMP_FAILURE_RETRY(read(sock, buffer, sizeof(buffer)));
    if (ret < 0) return -errno;

    return strncmp(buffer, success, sizeof(success) - 1) != 0;
}

// Foreground waits for exit of the main persistent threads
// that are started here. The threads are created to manage
// UNIX domain client sockets for writing, reading and
// controlling the user space logger, and for any additional
// logging plugins like auditd and restart control. Additional
// transitory per-client threads are created for each reader.
int main(int argc, char* argv[]) {
    // We want EPIPE when a reader disconnects, not to terminate logd.
    signal(SIGPIPE, SIG_IGN);
    // logd is written under the assumption that the timezone is UTC.
    // If TZ is not set, persist.sys.timezone is looked up in some time utility
    // libc functions, including mktime. It confuses the logd time handling,
    // so here explicitly set TZ to UTC, which overrides the property.
    setenv("TZ", "UTC", 1);
    // issue reinit command. KISS argument parsing.
    if ((argc > 1) && argv[1] && !strcmp(argv[1], "--reinit")) {
        return issueReinit();
    }

    android::base::InitLogging(
            argv, [](android::base::LogId log_id, android::base::LogSeverity severity,
                     const char* tag, const char* file, unsigned int line, const char* message) {
                if (tag && strcmp(tag, "logd") != 0) {
                    auto prefixed_message = android::base::StringPrintf("%s: %s", tag, message);
                    android::base::KernelLogger(log_id, severity, "logd", file, line,
                                                prefixed_message.c_str());
                } else {
                    android::base::KernelLogger(log_id, severity, "logd", file, line, message);
                }
            });

    static const char dev_kmsg[] = "/dev/kmsg";
    int fdDmesg = android_get_control_file(dev_kmsg);
    if (fdDmesg < 0) {
        fdDmesg = TEMP_FAILURE_RETRY(open(dev_kmsg, O_WRONLY | O_CLOEXEC));
    }

    int fdPmesg = -1;
    bool klogd_default =
            GetBoolProperty("ro.debuggable", false) && !GetBoolProperty("ro.config.low_ram", false);
    bool klogd = GetBoolProperty("ro.logd.kernel", klogd_default);
    if (klogd) {
        SetProperty("ro.logd.kernel", "true");
        static const char proc_kmsg[] = "/proc/kmsg";
        fdPmesg = android_get_control_file(proc_kmsg);
        if (fdPmesg < 0) {
            fdPmesg = TEMP_FAILURE_RETRY(
                open(proc_kmsg, O_RDONLY | O_NDELAY | O_CLOEXEC));
        }
        if (fdPmesg < 0) PLOG(ERROR) << "Failed to open " << proc_kmsg;
    }

    bool auditd = GetBoolProperty("ro.logd.auditd", true);
    DropPrivs(klogd, auditd);

    // A cache of event log tags
    LogTags log_tags;

    // Pruning configuration.
    PruneList prune_list;

    std::string buffer_type = GetProperty("logd.buffer_type", "serialized");

    LogStatistics log_statistics(false, buffer_type == "serialized");

    // Serves the purpose of managing the last logs times read on a socket connection, and as a
    // reader lock on a range of log entries.
    LogReaderList reader_list;

    // LogBuffer is the object which is responsible for holding all log entries.
    LogBuffer* log_buffer = nullptr;
    if (buffer_type == "serialized") {
        log_buffer = new SerializedLogBuffer(&reader_list, &log_tags, &log_statistics);
    } else if (buffer_type == "simple") {
        log_buffer = new SimpleLogBuffer(&reader_list, &log_tags, &log_statistics);
    } else {
        LOG(FATAL) << "buffer_type must be one of 'serialized' or 'simple'";
    }

    // LogReader listens on /dev/socket/logdr. When a client
    // connects, log entries in the LogBuffer are written to the client.
    LogReader* reader = new LogReader(log_buffer, &reader_list);
    if (reader->startListener()) {
        return EXIT_FAILURE;
    }

    // LogListener listens on /dev/socket/logdw for client
    // initiated log messages. New log entries are added to LogBuffer
    // and LogReader is notified to send updates to connected clients.
    LogListener* swl = new LogListener(log_buffer);
    if (!swl->StartListener()) {
        return EXIT_FAILURE;
    }

    // Command listener listens on /dev/socket/logd for incoming logd
    // administrative commands.
    CommandListener* cl = new CommandListener(log_buffer, &log_tags, &prune_list, &log_statistics);
    if (cl->startListener()) {
        return EXIT_FAILURE;
    }

    // Notify that others can now interact with logd
    SetProperty("logd.ready", "true");

    // LogAudit listens on NETLINK_AUDIT socket for selinux
    // initiated log messages. New log entries are added to LogBuffer
    // and LogReader is notified to send updates to connected clients.
    LogAudit* al = nullptr;
    if (auditd) {
        int dmesg_fd = GetBoolProperty("ro.logd.auditd.dmesg", true) ? fdDmesg : -1;
        al = new LogAudit(log_buffer, dmesg_fd);
    }

    LogKlog* kl = nullptr;
    if (klogd) {
        kl = new LogKlog(log_buffer, fdDmesg, fdPmesg, al != nullptr, &log_statistics);
    }

    readDmesg(al, kl);

    // failure is an option ... messages are in dmesg (required by standard)
    if (kl && kl->startListener()) {
        delete kl;
    }

    if (al && al->startListener()) {
        delete al;
    }

    TrustyLog::create(log_buffer);

    TEMP_FAILURE_RETRY(pause());

    return EXIT_SUCCESS;
}
