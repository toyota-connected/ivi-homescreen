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

#include <cmath>
#include <cstdint>

#include <shell/platform/embedder/embedder.h>

namespace homescreen {

// The extent a view resolved for itself, and the surface scale it reports that
// extent at. DRM and software adopt their mode's extent, which is already
// physical, at scale 1; a Wayland window reports logical dimensions plus the
// scale its compositor asked for (#150).
struct ViewExtent {
  int32_t width{0};
  int32_t height{0};
  double scale{1.0};
};

// The id of the one display this shell publishes.
//
// Every view's FlutterWindowMetricsEvent.display_id has to carry this same
// value. dart:ui's FlutterView.display looks the view's display id up in the
// map built from FlutterEngineNotifyDisplayUpdate and dereferences the result
// with `!`:
//
//     return platformDispatcher._displays[_viewConfiguration.displayId]!;
//
// so a view pointing at an id that was never published throws "Null check
// operator used on a null value" the first time anything asks that view for
// its display -- its refresh rate, for instance. That is latent until some
// package does ask: lottie 3.6.0 started scheduling frames at the composition
// rate rather than every vsync, which made it the first thing to notice that
// we published display 1 and told every view it was on display 0.
//
// Nothing in the embedder API reserves any value here; the two sides simply
// have to agree, so they share this constant instead of each spelling out a
// literal.
inline constexpr FlutterEngineDisplayId kPrimaryDisplayId = 1;

// The FlutterEngineDisplay a view reports to a running engine.
//
// @p backend_rate_hz is Backend::RefreshRateHz(): the rate the backend's own
// scanout mode paces to, 0 when it drives no scanout. @p display_rate_hz is
// the display's own figure -- the wl_output refresh on Wayland, a
// construction-time default on DRM and software -- so the backend wins
// wherever it has an answer, which is what #732 turned on.
//
// Shared by the start-up notify and every re-notify after an output change, so
// the two cannot report different shapes for the same view (#760).
[[nodiscard]] inline FlutterEngineDisplay MakeDisplayMetadata(
    const double backend_rate_hz,
    const double display_rate_hz,
    const ViewExtent extent,
    const double pixel_ratio) {
  FlutterEngineDisplay display{};
  display.struct_size = sizeof(FlutterEngineDisplay);
  display.display_id = kPrimaryDisplayId;
  display.single_display = true;
  display.refresh_rate =
      backend_rate_hz > 0.0 ? backend_rate_hz : display_rate_hz;
  display.width = static_cast<size_t>(
      std::lround(static_cast<double>(extent.width) * extent.scale));
  display.height = static_cast<size_t>(
      std::lround(static_cast<double>(extent.height) * extent.scale));
  display.device_pixel_ratio = pixel_ratio * extent.scale;
  return display;
}

}  // namespace homescreen
