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

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace homescreen {

// Which backend family is resolving — selects which name field of an
// OutputMatch is compared against an OutputInfo::name.
enum class BackendFamily {
  kWayland,  // compare OutputMatch::wl_name        (wl_output v4 name)
  kDrm,      // compare OutputMatch::drm_connector  (KMS connector name)
};

// A live (or, on DRM, enumerated-but-disconnected) physical output. `name` is
// the stable join key: the DRM connector name ("DP-1", "HDMI-A-1") or the
// wl_output v4 name. The other fields are descriptive (EDID / current mode).
struct OutputInfo {
  std::string name;
  std::string make;                   // EDID PNP id, e.g. "GSM"
  std::string model;                  // EDID model name
  std::optional<std::string> serial;  // EDID serial; DRM-only, often absent
  int32_t width_px = 0;
  int32_t height_px = 0;
  int32_t mm_width = 0;  // physical size from EDID
  int32_t mm_height = 0;
  double refresh_hz = 0.0;
  bool connected = false;  // DRM may enumerate a disconnected port
  uint64_t handle = 0;     // opaque: wl_output* / drm connector id
};

// Per-bundle output binding (Configuration::Config::view.output). The most
// specific field that matches a live output wins; see ResolveOutput.
struct OutputMatch {
  std::optional<std::string> wl_name;        // Wayland: wl_output name
  std::optional<std::string> drm_connector;  // DRM: kernel connector name
  std::optional<std::string> edid_serial;    // DRM-only refinement
  std::optional<uint32_t> index;             // deprecated last resort

  // Warm the engine (suspended, no scanout) at startup even if the target
  // output is absent, for instant first paint on plug-in. Costs held RAM/GPU.
  bool preload = false;

  // What to do with a running view when its output disappears.
  enum class OnDisconnect {
    kSuspend,   // keep the engine warm, stop presenting (instant replug)
    kTeardown,  // free the engine (cold-start on return)
  };
  OnDisconnect on_disconnect = OnDisconnect::kSuspend;

  // Position of this view's display in the combined pointer/desktop space, for
  // multi-display input routing. Unset = origin (0,0). Only meaningful when
  // several views share one input seat (multiple displays on one card): the
  // config declares each display's placement so the pointer moves between them
  // as laid out. Not a match field -- excluded from empty().
  std::optional<int32_t> layout_x;
  std::optional<int32_t> layout_y;

  // True when no match field is set: the bundle binds to the primary output
  // immediately (preserves single-view behavior). Layout position is not a
  // match constraint, so it does not affect this.
  [[nodiscard]] bool empty() const {
    return !wl_name && !drm_connector && !edid_serial && !index;
  }
};

// Resolve the OutputInfo::name a bundle should bind to, given the current live
// set, or nullopt if none matches (the bundle is then parked). `family` selects
// which OutputMatch name field is compared. Match order, most specific first:
//   1. edid_serial — DRM only; only if present AND it uniquely identifies one
//      connected output. A duplicate/ambiguous serial warns and falls through.
//   2. name        — drm_connector (DRM) or wl_name (Wayland); the primary
//      path, and what makes identical monitors follow their port.
//   3. index       — deprecated and unstable; warns. Indexes the connected
//      outputs in enumeration order.
//   4. none        — nullopt (parked).
// Only `connected` outputs are eligible (you cannot present to an absent port).
[[nodiscard]] std::optional<std::string> ResolveOutput(
    const OutputMatch& match,
    const std::vector<OutputInfo>& outputs,
    BackendFamily family);

}  // namespace homescreen
