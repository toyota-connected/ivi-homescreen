/*
 * Copyright 2026 Toyota Connected North America
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

// libinput → ihs::log priority bridge. Without a handler installed, libinput
// writes its own diagnostics (notably "client bug: event processing lagging
// behind by N ms", the marker the event-driven input work watches for) to its
// default stderr handler, bypassing the structured ihs::log / DLT fan-out.
// SoftwareSeat installs libinput_log_set_handler + _set_priority(INFO) and
// forwards through here.
//
// The priority→level map is factored out as a pure function so it is unit
// testable without a live libinput context (the "fake-priority forwarding"
// test): it only reads the enum, calls nothing.

#include <libinput.h>

#include "ihs/logging.h"

namespace homescreen {

// Maps a libinput log priority to the ihs::log level the line is forwarded at.
// libinput exposes exactly three priorities; anything outside them (a future
// libinput adding a level) surfaces at WARN so it is neither hidden nor
// mis-ranked as an error.
inline IhsLogLevel MapLibinputLogLevel(const libinput_log_priority priority) {
  switch (priority) {
    case LIBINPUT_LOG_PRIORITY_DEBUG:
      return IHS_LEVEL_DEBUG;
    case LIBINPUT_LOG_PRIORITY_INFO:
      return IHS_LEVEL_INFO;
    case LIBINPUT_LOG_PRIORITY_ERROR:
      return IHS_LEVEL_ERROR;
    default:
      return IHS_LEVEL_WARN;
  }
}

}  // namespace homescreen
