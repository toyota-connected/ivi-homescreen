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
  auto r = ResolveOutputDetailed(match, outputs, family);
  if (r.bound()) {
    return r.name;
  }
  return std::nullopt;
}

OutputResolution ResolveOutputDetailed(const OutputMatch& match,
                                       const std::vector<OutputInfo>& outputs,
                                       const BackendFamily family) {
  // Nothing asked for: the backend's own default is the right answer, and is
  // not the same as having asked and missed.
  if (match.empty()) {
    return {OutputResolution::Status::kUnconstrained, {}};
  }
  // 0. Role name (IHS_OUTPUT_NAME, set by an integrator's udev rule). The most
  //    specific tier and the only one portable across boards: the connector
  //    name and card number both move between hardware revisions, and a panel
  //    with no EDID has no serial to match on.
  //
  //    Unlike the tiers below, a miss here parks rather than falling through.
  //    Naming a role is a deliberate statement about which physical display a
  //    view belongs on; quietly binding it to a different one because the role
  //    was absent is worse than not binding at all -- an instrument cluster
  //    appearing on the passenger screen is not a graceful degradation.
  if (match.output_id) {
    const OutputInfo* hit = nullptr;
    int count = 0;
    for (const auto& o : outputs) {
      if (o.connected && o.output_id && *o.output_id == *match.output_id) {
        hit = &o;
        ++count;
      }
    }
    if (count == 1) {
      return {OutputResolution::Status::kBound, hit->name};
    }
    if (count > 1) {
      // Two connectors claiming one role is a mistake in the rule, and the
      // enumeration order that would decide it is not stable. Unlike an
      // ambiguous serial this does not fall through: the config named a role,
      // so silently binding by port instead would defeat the point.
      ihs::log::error(
          "[OutputResolver] output_id '{}' is claimed by {} connected outputs; "
          "the view is parked. A role must name one display -- check the udev "
          "rule setting IHS_OUTPUT_NAME",
          *match.output_id, count);
      return {OutputResolution::Status::kUnresolved, {}};
    }
    ihs::log::warn(
        "[OutputResolver] output_id '{}' matches no connected output; the view "
        "is parked. Check that a udev rule sets IHS_OUTPUT_NAME on the "
        "connector",
        *match.output_id);
    return {OutputResolution::Status::kUnresolved, {}};
  }

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
      return {OutputResolution::Status::kBound, hit->name};
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
        return {OutputResolution::Status::kBound, o.name};
      }
    }
    // Named but not currently connected: a constraint that is not satisfied,
    // which the caller must not confuse with having asked for nothing.
    return {OutputResolution::Status::kUnresolved, {}};
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
        return {OutputResolution::Status::kBound, o.name};
      }
      ++i;
    }
  }

  // 4. A constraint was set (match.empty() was false) and nothing satisfied it.
  return {OutputResolution::Status::kUnresolved, {}};
}

}  // namespace homescreen
