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

#include <cstddef>
#include <cstdint>

class TaskRunner;
typedef void (*VsyncCallback)(void*, intptr_t);

// Pluggable output destination for SoftwareBackend's
// surface_present_callback. Sinks own one direction of data — pixels in,
// either dropped, stored, or pushed to a device. They never call back
// into the backend.
//
// All Present() calls fire on Flutter's rasterizer thread. The buffer
// pointer is only valid for the duration of the call — copy if you
// need to retain. Pixel format is premultiplied RGBA8888;
// row stride is @p row_bytes (>= 4 * width), buffer size is
// row_bytes * height.
class ISurfaceSink {
 public:
  virtual ~ISurfaceSink() = default;

  ISurfaceSink(const ISurfaceSink&) = delete;
  ISurfaceSink& operator=(const ISurfaceSink&) = delete;

  // Per-frame callback. Returning false signals failure to the engine —
  // Flutter logs but continues. Most sinks should return true even on
  // soft errors (write retry pending, etc.) to avoid engine churn.
  virtual bool Present(const void* allocation,
                       size_t row_bytes,
                       size_t height) = 0;

  // Called once at backend construction (after the bundle's view
  // geometry is known) and on every Resize. Sinks that allocate their
  // own framebuffers (fbdev, drm-dumb) use this to (re)allocate.
  virtual void OnSize(uint32_t /*width*/, uint32_t /*height*/) {}

  // Optional vsync hooks for sinks with a real vblank source. The
  // default no-ops keep Flutter on its wall-clock scheduler.
  [[nodiscard]] virtual VsyncCallback GetVsyncCallback() const {
    return nullptr;
  }
  virtual void SetEngineHandle(void* /*engine*/) {}
  virtual void SetPlatformTaskRunner(TaskRunner* /*runner*/) {}
  virtual void StopVsyncMonitor() {}

 protected:
  ISurfaceSink() = default;
};
