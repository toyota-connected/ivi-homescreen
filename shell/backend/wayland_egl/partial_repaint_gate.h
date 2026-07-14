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

// Partial-repaint capability gate for the Wayland/EGL backend.
//
// The backend's partial-repaint path (buffer-age existing-damage query +
// eglSetDamageRegionKHR + eglSwapBuffersWithDamageKHR) is correct only under
// the Skia OpenGL renderer, where the engine both consumes the existing damage
// and reports frame/buffer damage back on present. Under Impeller GLES the
// engine still queries existing damage and advertises partial-repaint support,
// but its present path reports no damage: feeding it buffer-age-narrowed
// existing damage would let it clip rendering against history the presented
// buffer never actually preserved. The backend therefore reports the whole
// surface as damaged (full repaint) whenever Impeller is the active renderer.
//
// The shell has no dedicated Impeller flag; Impeller is requested through the
// generic engine-switch passthrough (--enable-impeller), so the active renderer
// is inferred from the forwarded switches. The parse is a pure function, kept
// here so it is unit testable without an engine.

#include <string>
#include <string_view>
#include <vector>

// True if the forwarded engine switches request Impeller. Recognizes the bare
// "--enable-impeller" flag and the "--enable-impeller=<value>" form; a value of
// "false" or "0" disables it. The last matching switch wins.
//
// Free function in the global namespace to match the Wayland/EGL backend it
// supports (WaylandEglBackend and its factory are global-namespace).
inline bool EngineSwitchesEnableImpeller(
    const std::vector<std::string>& switches) {
  constexpr std::string_view kKey = "--enable-impeller";
  bool enabled = false;
  for (const auto& s : switches) {
    const std::string_view v(s);
    if (v == kKey) {
      enabled = true;
    } else if (v.size() > kKey.size() && v.substr(0, kKey.size()) == kKey &&
               v[kKey.size()] == '=') {
      const std::string_view value = v.substr(kKey.size() + 1);
      enabled = value != "false" && value != "0";
    }
  }
  return enabled;
}
