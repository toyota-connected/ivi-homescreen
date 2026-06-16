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

namespace homescreen {

// Minimal sink the DRM seat pushes pointer positions (in render/viewport
// pixels) to, for a composited cursor that has no hardware plane — e.g. the
// EGL backend's GlCursor on display controllers (i.MX LCDIF, etc.) that expose
// only a primary plane. It is a pure-virtual interface so DrmSeat (shared with
// the Vulkan DRM backend) carries no link dependency on any concrete cursor:
// a backend that never sets one leaves the seat's pointer null and the call is
// skipped.
class ICursorPositionSink {
 public:
  virtual ~ICursorPositionSink() = default;

  // Place the cursor hotspot at (x, y) in render/viewport pixels and mark it
  // visible. Called from the seat's input-dispatch thread.
  virtual void SetPosition(int32_t x, int32_t y) = 0;
};

}  // namespace homescreen
