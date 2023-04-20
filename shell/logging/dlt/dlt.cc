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

#include "constants.h"

static DltContext gContext{};
static bool gContextSet = false;

bool Dlt::IsSupported() {
  return LibDlt::IsPresent();
}

bool Dlt::Register() {
  char app_id_[5]{};

  if (DltReturnValue::Ok != LibDlt->GetAppId(app_id_))
    return false;

  if (app_id_[0] == 0) {
    if (DltReturnValue::Ok !=
        LibDlt->RegisterApp(kDltAppId, kDltAppIdDescription)) {
      return false;
    }

    if (!gContextSet) {
      if (DltReturnValue::Ok !=
          LibDlt->RegisterContext(&gContext, kDltContextId,
                                  kDltContextIdDescription)) {
        return false;
      }
      gContextSet = true;
    }
  }

  return true;
}

bool Dlt::Unregister() {
  if (gContextSet) {
    if (DltReturnValue::Ok != LibDlt->UnregisterContext(&gContext)) {
      return false;
    }
    memset(&gContext, 0, sizeof(DltContext));
    gContextSet = false;
  }

  if (DltReturnValue::Ok != LibDlt->UnregisterApp()) {
    return false;
  }

  return true;
}

MAYBE_UNUSED
void Dlt::LogString(DltLogLevelType log_level, const char* buff) {
  if (gContextSet) {
    DltContextData log_local;
    if (DltReturnValue::True ==
        LibDlt->UserLogWriteStart(&gContext, &log_local, log_level)) {
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
