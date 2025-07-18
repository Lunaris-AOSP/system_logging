/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "LogAudit.h"

#include <ctype.h>
#include <endian.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/uio.h>
#include <syslog.h>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/macros.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <private/android_filesystem_config.h>
#include <private/android_logger.h>

#include "LogKlog.h"
#include "LogUtils.h"
#include "libaudit.h"

using namespace std::string_literals;

using android::base::GetBoolProperty;

#define KMSG_PRIORITY(PRI)                               \
    '<', '0' + LOG_MAKEPRI(LOG_AUTH, LOG_PRI(PRI)) / 10, \
        '0' + LOG_MAKEPRI(LOG_AUTH, LOG_PRI(PRI)) % 10, '>'

LogAudit::LogAudit(LogBuffer* buf, int fdDmesg)
    : SocketListener(getLogSocket(), false),
      logbuf(buf),
      fdDmesg(fdDmesg),
      main(GetBoolProperty("ro.logd.auditd.main", true)),
      events(GetBoolProperty("ro.logd.auditd.events", true)),
      initialized(false) {
    static const char auditd_message[] = { KMSG_PRIORITY(LOG_INFO),
                                           'l',
                                           'o',
                                           'g',
                                           'd',
                                           '.',
                                           'a',
                                           'u',
                                           'd',
                                           'i',
                                           't',
                                           'd',
                                           ':',
                                           ' ',
                                           's',
                                           't',
                                           'a',
                                           'r',
                                           't',
                                           '\n' };
    write(fdDmesg, auditd_message, sizeof(auditd_message));
}

bool LogAudit::onDataAvailable(SocketClient* cli) {
    if (!initialized) {
        prctl(PR_SET_NAME, "logd.auditd");
        initialized = true;
    }

    struct audit_message rep;

    rep.nlh.nlmsg_type = 0;
    rep.nlh.nlmsg_len = 0;
    rep.data[0] = '\0';

    if (audit_get_reply(cli->getSocket(), &rep, GET_REPLY_BLOCKING, 0) < 0) {
        SLOGE("Failed on audit_get_reply with error: %s", strerror(errno));
        return false;
    }

    logPrint("type=%d %.*s", rep.nlh.nlmsg_type, rep.nlh.nlmsg_len, rep.data);

    return true;
}

static inline bool hasMetadata(char* str, int str_len) {
    // need to check and see if str already contains bug metadata from
    // possibility of stuttering if log audit crashes and then reloads kernel
    // messages. Kernel denials that contain metadata will either end in
    // "b/[0-9]+$" or "b/[0-9]+  duplicate messages suppressed$" which will put
    // a '/' character at either 9 or 39 indices away from the end of the str.
    return str_len >= 39 &&
           (str[str_len - 9] == '/' || str[str_len - 39] == '/');
}

static auto populateDenialMap() {
    std::map<std::tuple<std::string, std::string, std::string>, std::string> denial_to_bug;
    // Order matters. Only the first occurrence of a
    // (scontext, tcontext, tclass) combination is recorded.
    for (const auto& bug_map_file :
         {"/system_ext/etc/selinux/bug_map"s, "/vendor/etc/selinux/selinux_denial_metadata"s,
          "/system/etc/selinux/bug_map"s}) {
        std::string file_contents;
        if (!android::base::ReadFileToString(bug_map_file, &file_contents)) {
            continue;
        }
        int errors = 0;
        for (const auto& line : android::base::Split(file_contents, "\n")) {
            const auto fields = android::base::Tokenize(line, " ");
            if (fields.empty() || android::base::StartsWith(fields.front(), '#')) {
                continue;
            }
            if (fields.size() == 4) {
                const std::string& scontext = fields[0];
                const std::string& tcontext = fields[1];
                const std::string& tclass = fields[2];
                const std::string& bug_num = fields[3];
                const auto [it, success] =
                        denial_to_bug.try_emplace({scontext, tcontext, tclass}, bug_num);
                if (!success) {
                    const auto& [key, value] = *it;
                    LOG(WARNING) << "Ignored bug_map definition in " << bug_map_file << ": '"
                                 << line
                                 << "', (scontext, tcontext, tclass) denial combination is already "
                                    "tagged with bug metadata '"
                                 << value << "'";
                }
            } else {
                LOG(ERROR) << "Ignored ill-formed bug_map definition in " << bug_map_file << ": '"
                           << line << "'";
                ++errors;
            }
        }
        if (errors) {
            LOG(ERROR) << "Loaded bug_map file with " << errors << " errors: " << bug_map_file;
        } else {
            LOG(INFO) << "Loaded bug_map file: " << bug_map_file;
        }
    }
    return denial_to_bug;
}

std::string LogAudit::denialParse(const std::string& denial, char terminator,
                                  const std::string& search_term) {
    size_t start_index = denial.find(search_term);
    if (start_index != std::string::npos) {
        start_index += search_term.length();
        return denial.substr(
            start_index, denial.find(terminator, start_index) - start_index);
    }
    return "";
}

std::string LogAudit::auditParse(const std::string& string, uid_t uid) {
    // Allocate a static map object to memoize the loaded bug_map files.
    static auto denial_to_bug = populateDenialMap();

    std::string result;
    std::string scontext = denialParse(string, ':', "scontext=u:object_r:");
    std::string tcontext = denialParse(string, ':', "tcontext=u:object_r:");
    std::string tclass = denialParse(string, ' ', "tclass=");
    if (scontext.empty()) {
        scontext = denialParse(string, ':', "scontext=u:r:");
    }
    if (tcontext.empty()) {
        tcontext = denialParse(string, ':', "tcontext=u:r:");
    }
    auto search = denial_to_bug.find({scontext, tcontext, tclass});
    if (search != denial_to_bug.end()) {
        result = " bug=" + search->second;
    }

    // Ensure the uid name is not null before passing it to the bug string.
    if (uid >= AID_APP_START && uid <= AID_APP_END) {
        char* uidname = android::uidToName(uid);
        if (uidname) {
            result.append(" app="s + uidname);
            free(uidname);
        }
    }
    return result;
}

int LogAudit::logPrint(const char* fmt, ...) {
    if (fmt == nullptr) {
        return -EINVAL;
    }

    va_list args;

    char* str = nullptr;
    va_start(args, fmt);
    int rc = vasprintf(&str, fmt, args);
    va_end(args);

    if (rc < 0) {
        return rc;
    }
    char* cp;
    // Work around kernels missing
    // https://github.com/torvalds/linux/commit/b8f89caafeb55fba75b74bea25adc4e4cd91be67
    // Such kernels improperly add newlines inside audit messages.
    while ((cp = strchr(str, '\n'))) {
        *cp = ' ';
    }

    pid_t pid = getpid();
    pid_t tid = gettid();
    uid_t uid = AID_LOGD;
    static const char pid_str[] = " pid=";
    char* pidptr = strstr(str, pid_str);
    if (pidptr && isdigit(pidptr[sizeof(pid_str) - 1])) {
        cp = pidptr + sizeof(pid_str) - 1;
        pid = 0;
        while (isdigit(*cp)) {
            pid = (pid * 10) + (*cp - '0');
            ++cp;
        }
        tid = pid;
        uid = android::pidToUid(pid);
        memmove(pidptr, cp, strlen(cp) + 1);
    }

    bool info = strstr(str, " permissive=1") || strstr(str, " policy loaded ");
    static std::string denial_metadata;
    if ((fdDmesg >= 0) && initialized) {
        struct iovec iov[4];
        static const char log_info[] = { KMSG_PRIORITY(LOG_INFO) };
        static const char log_warning[] = { KMSG_PRIORITY(LOG_WARNING) };
        static const char newline[] = "\n";

        denial_metadata = auditParse(str, uid);
        iov[0].iov_base = info ? const_cast<char*>(log_info) : const_cast<char*>(log_warning);
        iov[0].iov_len = info ? sizeof(log_info) : sizeof(log_warning);
        iov[1].iov_base = str;
        iov[1].iov_len = strlen(str);
        iov[2].iov_base = const_cast<char*>(denial_metadata.c_str());
        iov[2].iov_len = denial_metadata.length();
        iov[3].iov_base = const_cast<char*>(newline);
        iov[3].iov_len = strlen(newline);

        writev(fdDmesg, iov, arraysize(iov));
    }

    if (!main && !events) {
        free(str);
        return 0;
    }

    log_time now(log_time::EPOCH);

    static const char audit_str[] = " audit(";
    char* timeptr = strstr(str, audit_str);
    if (timeptr && ((cp = now.strptime(timeptr + sizeof(audit_str) - 1, "%s.%q"))) &&
        (*cp == ':')) {
        memcpy(timeptr + sizeof(audit_str) - 1, "0.0", 3);
        memmove(timeptr + sizeof(audit_str) - 1 + 3, cp, strlen(cp) + 1);
    } else {
        now = log_time(CLOCK_REALTIME);
    }

    // log to events

    size_t str_len = strnlen(str, LOGGER_ENTRY_MAX_PAYLOAD);
    if (((fdDmesg < 0) || !initialized) && !hasMetadata(str, str_len))
        denial_metadata = auditParse(str, uid);
    str_len = (str_len + denial_metadata.length() <= LOGGER_ENTRY_MAX_PAYLOAD)
                  ? str_len + denial_metadata.length()
                  : LOGGER_ENTRY_MAX_PAYLOAD;
    size_t message_len = str_len + sizeof(android_log_event_string_t);

    unsigned int notify = 0;

    if (events) {  // begin scope for event buffer
        uint32_t buffer[(message_len + sizeof(uint32_t) - 1) / sizeof(uint32_t)];

        android_log_event_string_t* event =
            reinterpret_cast<android_log_event_string_t*>(buffer);
        event->header.tag = htole32(AUDITD_LOG_TAG);
        event->type = EVENT_TYPE_STRING;
        event->length = htole32(str_len);
        memcpy(event->data, str, str_len - denial_metadata.length());
        memcpy(event->data + str_len - denial_metadata.length(),
               denial_metadata.c_str(), denial_metadata.length());

        rc = logbuf->Log(LOG_ID_EVENTS, now, uid, pid, tid, reinterpret_cast<char*>(event),
                         (message_len <= UINT16_MAX) ? (uint16_t)message_len : UINT16_MAX);
        if (rc >= 0) {
            notify |= 1 << LOG_ID_EVENTS;
        }
        // end scope for event buffer
    }

    // log to main

    static const char comm_str[] = " comm=\"";
    const char* comm = strstr(str, comm_str);
    const char* estr = str + strlen(str);
    const char* commfree = nullptr;
    if (comm) {
        estr = comm;
        comm += sizeof(comm_str) - 1;
    } else if (pid == getpid()) {
        pid = tid;
        comm = "auditd";
    } else {
        comm = commfree = android::pidToName(pid);
        if (!comm) {
            comm = "unknown";
        }
    }

    const char* ecomm = strchr(comm, '"');
    if (ecomm) {
        ++ecomm;
        str_len = ecomm - comm;
    } else {
        str_len = strlen(comm) + 1;
        ecomm = "";
    }
    size_t prefix_len = estr - str;
    if (prefix_len > LOGGER_ENTRY_MAX_PAYLOAD) {
        prefix_len = LOGGER_ENTRY_MAX_PAYLOAD;
    }
    size_t suffix_len = strnlen(ecomm, LOGGER_ENTRY_MAX_PAYLOAD - prefix_len);
    message_len =
        str_len + prefix_len + suffix_len + denial_metadata.length() + 2;

    if (main) {  // begin scope for main buffer
        char newstr[message_len];

        *newstr = info ? ANDROID_LOG_INFO : ANDROID_LOG_WARN;
        strlcpy(newstr + 1, comm, str_len);
        strncpy(newstr + 1 + str_len, str, prefix_len);
        strncpy(newstr + 1 + str_len + prefix_len, ecomm, suffix_len);
        strncpy(newstr + 1 + str_len + prefix_len + suffix_len,
                denial_metadata.c_str(), denial_metadata.length());

        rc = logbuf->Log(LOG_ID_MAIN, now, uid, pid, tid, newstr,
                         (message_len <= UINT16_MAX) ? (uint16_t)message_len : UINT16_MAX);

        if (rc >= 0) {
            notify |= 1 << LOG_ID_MAIN;
        }
        // end scope for main buffer
    }

    free(const_cast<char*>(commfree));
    free(str);

    if (notify) {
        if (rc < 0) {
            rc = message_len;
        }
    }

    return rc;
}

int LogAudit::log(char* buf, size_t len) {
    char* audit = strstr(buf, " audit(");
    if (!audit || (audit >= &buf[len])) {
        return 0;
    }

    *audit = '\0';

    int rc;
    char* type = strstr(buf, "type=");
    if (type && (type < &buf[len])) {
        rc = logPrint("%s %s", type, audit + 1);
    } else {
        rc = logPrint("%s", audit + 1);
    }
    *audit = ' ';
    return rc;
}

int LogAudit::getLogSocket() {
    int fd = audit_open();
    if (fd < 0) {
        return fd;
    }
    if (audit_setup(fd, getpid()) < 0) {
        audit_close(fd);
        fd = -1;
    }
    return fd;
}
