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

#pragma once

#include <map>

#include <sysutils/SocketListener.h>

#include "LogBuffer.h"
#include "LogStatistics.h"

class LogAudit : public SocketListener {
    LogBuffer* logbuf;
    int fdDmesg;  // fdDmesg >= 0 is functionally bool dmesg
    bool main;
    bool events;
    bool initialized;

  public:
    LogAudit(LogBuffer* buf, int fdDmesg);
    int log(char* buf, size_t len);

  protected:
    virtual bool onDataAvailable(SocketClient* cli);

  private:
    static int getLogSocket();
    std::string denialParse(const std::string& denial, char terminator,
                            const std::string& search_term);
    std::string auditParse(const std::string& string, uid_t uid);
    int logPrint(const char* fmt, ...)
        __attribute__((__format__(__printf__, 2, 3)));
};
