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

#include "display/output.h"

#include "logging/logging.h"

namespace homescreen {

std::optional<std::string> ResolveOutput(const OutputMatch& match,
                                         const std::vector<OutputInfo>& outputs,
                                         const BackendFamily family) {
  // 1. EDID serial (DRM only) — bind only if the serial identifies exactly one
  //    connected output. Identical monitors share make/model and usually carry
  //    no unique serial, so an ambiguous match falls through to the port name.
  if (family == BackendFamily::kDrm && match.edid_serial) {
    const OutputInfo* hit = nullptr;
    int count = 0;
    for (const auto& o : outputs) {
      if (o.connected && o.serial && *o.serial == *match.edid_serial) {
        hit = &o;
        ++count;
      }
    }
    if (count == 1) {
      return hit->name;
    }
    if (count > 1) {
      ihs::log::warn(
          "[OutputResolver] edid_serial '{}' matches {} connected outputs; "
          "ambiguous — falling back to the connector name",
          *match.edid_serial, count);
    }
    // count == 0 → the panel is absent (or on a port matched by name below).
  }

  // 2. Name — the primary path. drm_connector on DRM, wl_name on Wayland. This
  //    makes identical monitors follow their physical port.
  const std::optional<std::string>& want =
      (family == BackendFamily::kWayland) ? match.wl_name : match.drm_connector;
  if (want) {
    for (const auto& o : outputs) {
      if (o.connected && o.name == *want) {
        return o.name;
      }
    }
    // Named but not currently connected → parked (fall through to nullopt).
    return std::nullopt;
  }

  // 3. Index — deprecated and unstable; the enumeration order can shift across
  //    probes/reboots. Indexes the connected outputs in order.
  if (match.index) {
    ihs::log::warn(
        "[OutputResolver] output.index is deprecated and unstable; prefer a "
        "connector / wl_output name");
    uint32_t i = 0;
    for (const auto& o : outputs) {
      if (!o.connected) {
        continue;
      }
      if (i == *match.index) {
        return o.name;
      }
      ++i;
    }
  }

  // 4. Nothing matched → parked.
  return std::nullopt;
}

}  // namespace homescreen
