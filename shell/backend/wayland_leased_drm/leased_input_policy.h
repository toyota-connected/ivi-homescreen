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

#include <string_view>

namespace homescreen {

// Whether a leased backend reads input from raw evdev, and why.
//
// A leased client has no wl_surface, so the compositor delivers it no input and
// its only source is libinput on /dev/input/event*. Those reads are ungrabbed:
// on a host with a live Wayland session every event reaches BOTH this process
// and the session compositor (input is duplicated, not stolen). This is the
// pure policy that decides what to do about it; session detection, logging, and
// the config read live in register_backends.cc, which is why this stays a
// dependency-free header (so the decision can be unit-tested directly).
enum class LeasedInputDecision {
  kEnabledAuto,           // auto + no host session: an embedded target.
  kForcedOn,              // lease_input=on: read evdev even under a session.
  kDisabledByConfig,      // lease_input=off.
  kDisabledUnderSession,  // auto + a host session is present.
};

// True when the decision means "start the seat / open input devices".
[[nodiscard]] inline bool LeasedInputEnabled(LeasedInputDecision d) {
  return d == LeasedInputDecision::kEnabledAuto ||
         d == LeasedInputDecision::kForcedOn;
}

// Classify a lease_input mode string. Recognized: off|none|false|0 (disable),
// on|yes|true|1 (force on). Anything else -- including "auto", empty, and
// unrecognized values -- is auto: enabled on an embedded target, disabled when
// a host session is present. @p session_present is WaylandSessionAvailable().
[[nodiscard]] inline LeasedInputDecision DecideLeasedInput(
    std::string_view mode,
    bool session_present) {
  if (mode == "off" || mode == "none" || mode == "false" || mode == "0") {
    return LeasedInputDecision::kDisabledByConfig;
  }
  if (mode == "on" || mode == "yes" || mode == "true" || mode == "1") {
    return LeasedInputDecision::kForcedOn;
  }
  return session_present ? LeasedInputDecision::kDisabledUnderSession
                         : LeasedInputDecision::kEnabledAuto;
}

// True when @p mode is neither a recognized on/off value nor "auto"/empty --
// i.e. a typo the caller should warn about before falling back to auto.
[[nodiscard]] inline bool IsUnrecognizedLeasedInputMode(std::string_view mode) {
  return DecideLeasedInput(mode, /*session_present=*/false) ==
             LeasedInputDecision::kEnabledAuto &&
         !(mode.empty() || mode == "auto");
}

}  // namespace homescreen
