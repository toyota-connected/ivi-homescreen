/*
 * Copyright 2023 Toyota Connected North America
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

#include "dlt.h"

#include <cstring>
#include <iostream>

#include "config.h"

static DltContext gContext{};
static bool gContextSet = false;

bool Dlt::IsSupported() {
  return LibDlt::IsPresent();
}

bool Dlt::Register() {
  auto res = LibDlt->CheckLibraryVersion(DLT_PACKAGE_MAJOR_VERSION,
                                         DLT_PACKAGE_MINOR_VERSION);
  if (res != DltReturnValue::Ok) {
    std::cerr << "[DLT] lib version != " << DLT_PACKAGE_MAJOR_VERSION << "."
              << DLT_PACKAGE_MINOR_VERSION << std::endl;
  }
  res = LibDlt->RegisterApp(kDltAppId, kDltAppIdDescription);
  if (res != DltReturnValue::Ok) {
    std::cerr << "[DLT] RegisterApp: " << res << std::endl;
  }

  if (!gContextSet) {
    if (DltReturnValue::Ok !=
        LibDlt->RegisterContext(&gContext, kDltContextId,
                                kDltContextIdDescription)) {
      std::cerr << "[DLT] Failed to register context" << std::endl;
      return false;
    }
    gContextSet = true;
  }

  return true;
}

bool Dlt::Unregister() {
  auto res = LibDlt->UnregisterContext(&gContext);
  if (res != DltReturnValue::Ok) {
    std::cerr << "[DLT] unregister context failed" << std::endl;
  }
  memset(&gContext, 0, sizeof(DltContext));
  gContextSet = false;

  res = LibDlt->UnregisterApp();
  if (res != DltReturnValue::Ok) {
    std::cerr << "[DLT] UnregisterApp: " << res << std::endl;
  }

  return true;
}

MAYBE_UNUSED
void Dlt::LogString(DltLogLevelType log_level, const char* buff) {
  if (gContextSet) {
    DltContextData log_local;
    auto res = LibDlt->UserLogWriteStart(&gContext, &log_local, log_level);
    if (res == DltReturnValue::True) {
      (void)LibDlt->UserLogWriteString(&log_local, buff);
      (void)LibDlt->UserLogWriteFinish(&log_local);
    }

    if (log_level == DltLogLevelType::LOG_FATAL) {
      Dlt::Unregister();
    }
  } else {
    std::cerr << buff;
    std::cerr.flush();
  }
}

MAYBE_UNUSED
void Dlt::LogSizedString(DltLogLevelType log_level,
                         const char* buff,
                         uint16_t length) {
  if (gContextSet && length == 0) {
    LogString(log_level, buff);
  } else if (gContextSet && length) {
    DltContextData log_local;
    auto res = LibDlt->UserLogWriteStart(&gContext, &log_local, log_level);
    if (res == DltReturnValue::True) {
      (void)LibDlt->UserLogWriteSizedUtf8String(&log_local, buff, length);
      (void)LibDlt->UserLogWriteFinish(&log_local);
    }

    if (log_level == DltLogLevelType::LOG_FATAL) {
      Dlt::Unregister();
    }
  } else {
    std::cerr << buff;
    std::cerr.flush();
  }
}
