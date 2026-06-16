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

struct FlutterDesktopViewControllerState;

namespace homescreen {

// Lifecycle contract for an input source that delivers keyboard / pointer /
// touch events into a running Flutter engine. Concrete implementations pick
// the event source (libinput, wl_seat, …) and pair with whichever rendering
// backend is compiled in. The DRM-rendering path defaults to DrmSeat
// (libinput) but a Wayland-client + DRM-rendering combination would plug in
// a WaylandSeat here instead.
class ISeat {
 public:
  virtual ~ISeat() = default;
  ISeat(const ISeat&) = delete;
  ISeat& operator=(const ISeat&) = delete;

  // Opens the event source and starts dispatching. Returns false on
  // initialization failure (missing permissions, missing session, etc.).
  virtual bool Start() = 0;

  // Signals dispatch to stop and joins any worker thread. Safe to call
  // multiple times.
  virtual void Stop() = 0;

  // The seat reaches the running FlutterEngine through this pointer. Safe to
  // call at any time, including before the engine is running; nullptr pauses
  // delivery.
  virtual void SetViewControllerState(
      FlutterDesktopViewControllerState* state) = 0;

 protected:
  ISeat() = default;
};

}  // namespace homescreen
