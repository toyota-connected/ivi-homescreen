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

// Which renderer the forwarded engine switches select.
//
// Engine switches are forwarded verbatim to the engine's command_line_argv, so
// the shell learns the active renderer by reading what it passed on rather than
// by asking the engine. Two backends need that answer, for opposite reasons:
//
//   - wayland_egl gates partial repaint on it. Its buffer-age existing-damage
//     query + eglSwapBuffersWithDamageKHR path is correct only under Skia GL,
//     where the engine both consumes existing damage and reports damage back on
//     present. Impeller GLES queries existing damage and advertises
//     partial-repaint support but reports none on present, so feeding it
//     buffer-age-narrowed damage would let it clip against history the
//     presented buffer never preserved. That backend reports a full repaint
//     whenever Impeller is active.
//
//   - drm_kms_vulkan refuses outright. Impeller's Vulkan backend requires a
//     window-system surface stack; a KMS-direct backend owns scanout itself,
//     creates no VkSurfaceKHR and declares no WSI extensions, so the context
//     fails to initialize and nothing renders.
//
// The parse is a pure function, kept in its own header so it is unit testable
// without an engine and reachable from any backend regardless of which others
// are compiled in.

#include <string>
#include <string_view>
#include <vector>

// True if the forwarded engine switches request Impeller. Recognizes the bare
// "--enable-impeller" flag and the "--enable-impeller=<value>" form; a value of
// "false" or "0" disables it. The last matching switch wins.
//
// Free function in the global namespace to match the backend factories that
// call it, which are global-namespace.
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
