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
#include <memory>
#include <optional>
#include <utility>

class TaskRunner;
class SoftwareCursor;
typedef void (*VsyncCallback)(void*, intptr_t);

namespace profiling {
class MotionToPhoton;
}

// Pluggable output destination for SoftwareBackend's
// surface_present_callback. Sinks own one direction of data — pixels in,
// either dropped, stored, or pushed to a device. They never call back
// into the backend.
//
// All Present() calls fire on Flutter's rasterizer thread. The buffer
// pointer is only valid for the duration of the call — copy if you
// need to retain it. Pixel format is Skia's kN32_SkColorType:
// little-endian hosts deliver premultiplied BGRA8888 (memory bytes
// [B, G, R, A]); big-endian hosts deliver RGBA8888. Sinks targeting a
// named on-wire format should route through `pixel_swizzle.h` so the
// endian gate stays in one place. Row stride is @p row_bytes (>= 4 *
// width), buffer size is row_bytes * height.
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

  // Install the software mouse cursor (shared with the seat, which updates its
  // position). Device sinks (drm-dumb) composite it onto each frame; the
  // headless sinks (file/memory/none) inherit this no-op.
  virtual void SetCursor(std::shared_ptr<SoftwareCursor> /*cursor*/) {}

  // The sink's native output extent (the DRM mode / fbdev virtual size),
  // when it drives a fixed-size display. The SoftwareBackend adopts this as
  // the engine viewport + seat coordinate space so Flutter renders at the
  // panel's native resolution (fills + centers, and the pointer/cursor share
  // the framebuffer's coordinate space). nullopt for sinks with no inherent
  // size (file / memory / none).
  [[nodiscard]] virtual std::optional<std::pair<uint32_t, uint32_t>>
  NativeSize() const {
    return std::nullopt;
  }

  // True, when the sink has a real vblank source, it can drive Flutter's
  // vsync_callback from. SoftwareBackend's GetVsyncCallback() returns
  // a trampoline iff this is true.
  [[nodiscard]] virtual bool SupportsVsync() const { return false; }

  // Forwarded from SoftwareBackend's VsyncTrampoline. Sinks that
  // override SupportsVsync() store the baton and either return it
  // inline (idle-kick) or hand it back later from their vblank event
  // handler. Default no-op.
  virtual void SubmitBaton(void* /*engine*/, intptr_t /*baton*/) {}

  // Backend handle / runner plumbing. SoftwareBackend forwards these
  // unconditionally; sinks that don't need them inherit no-ops.
  virtual void SetEngineHandle(void* /*engine*/) {}
  virtual void SetPlatformTaskRunner(TaskRunner* /*runner*/) {}
  virtual void StopVsyncMonitor() {}

  // Hand the backend's motion-to-photon profiler (or nullptr) to a sink that
  // drives page flips, so it can record the scanout endpoint. Default no-op.
  virtual void SetMotionToPhoton(profiling::MotionToPhoton* /*m2p*/) {}

 protected:
  ISurfaceSink() = default;
};
