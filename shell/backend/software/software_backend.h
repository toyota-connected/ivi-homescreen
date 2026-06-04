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

#include <atomic>
#include <cstdint>
#include <memory>

#include "backend/backend.h"
#include "backend/software/surface_sink.h"

class Engine;

// CPU-rendering backend. Wires Flutter's kSoftware renderer config to
// a pluggable ISurfaceSink. No GPU, no display server, no Wayland.
class SoftwareBackend final : public Backend {
 public:
  SoftwareBackend(uint32_t initial_width,
                  uint32_t initial_height,
                  std::unique_ptr<ISurfaceSink> sink);

  // dtor emits the IVI_SW_PROFILE session summary when the env var is
  // set. Order matters: backend destructs before the sink so the
  // summary log captures the full run regardless of which sink was
  // active. Defined in the .cc.
  ~SoftwareBackend() override;

  void Resize(size_t index,
              Engine* flutter_engine,
              int32_t width,
              int32_t height) override;

  // wl_surface argument is inherited from the Backend interface; ignored
  // here — there is no Wayland surface in this backend.
  void CreateSurface(size_t index,
                     wl_surface* surface,
                     int32_t width,
                     int32_t height) override;

  bool TextureMakeCurrent() override { return true; }
  bool TextureClearCurrent() override { return true; }

  FlutterRendererConfig GetRenderConfig() override;
  FlutterCompositor GetCompositorConfig() override;

  // Resolved framebuffer extent. Adopted from the sink's NativeSize() (the DRM
  // mode / fbdev virtual size) when the sink drives a fixed-size display, else
  // the config view size. FlutterView reads these to drive the engine viewport
  // + seat coordinate space, mirroring the DRM backends' width()/height().
  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }

  // Forward the shared software cursor to the sink (the dumb sink composites
  // it). Called by FlutterView with the SoftwareDisplay-owned cursor. No-op for
  // headless sinks. (SoftwareCursor is forward-declared via surface_sink.h.)
  void SetCursor(std::shared_ptr<SoftwareCursor> cursor) {
    if (sink_) {
      sink_->SetCursor(std::move(cursor));
    }
  }

  // Vsync wiring is gated on the sink advertising a real vblank source
  // (DRM dumb buffer is the only sink that currently does) AND
  // IVI_SW_VSYNC not being "0". Sinks without a vblank source return
  // false from SupportsVsync(); the IVI_SW_VSYNC=0 kill-switch lets
  // bench runs A/B between the wired and wall-clock-only paths.
  // Either gate failing → nullptr → Flutter falls back to its
  // internal wall-clock scheduler.
  [[nodiscard]] VsyncCallback GetVsyncCallback() const override;
  void SetEngineHandle(FLUTTER_API_SYMBOL(FlutterEngine) engine) override {
    if (sink_) {
      sink_->SetEngineHandle(static_cast<void*>(engine));
    }
  }
  void SetPlatformTaskRunner(TaskRunner* runner) override {
    if (sink_) {
      sink_->SetPlatformTaskRunner(runner);
    }
  }
  void StopVsyncMonitor() override {
    if (sink_) {
      sink_->StopVsyncMonitor();
    }
  }

 private:
  static bool PresentTrampoline(void* user_data,
                                const void* allocation,
                                size_t row_bytes,
                                size_t height);

  // Flutter's vsync_callback entry point. Recovers the SoftwareBackend
  // from the FlutterDesktopEngineState* user_data and forwards the
  // baton to the sink, which owns the vblank-driven baton lifecycle.
  static void VsyncTrampoline(void* user_data, intptr_t baton);

  // Per-frame cadence profile (IVI_SW_PROFILE=1). CLOCK_MONOTONIC
  // sampled immediately after each successful sink->Present(); the
  // rasterizer thread is the only writer, so no locks. Same bucket
  // thresholds as the wayland_egl / wayland_vulkan profilers
  // (≤17ms / 18-33ms / 34-50ms / 51-100ms / >100ms) so histograms
  // line up across backends. For sinks that have no real vblank
  // source (file/memory/fbdev) the histogram measures Flutter's
  // wall-clock pacing; the DRM dumb sink's strict ping-pong + page-
  // flip wait makes the same metric reflect actual scanout cadence.
  struct FrameProfile {
    uint64_t last_present_ns{0};
    uint64_t interval_sum_ns{0};
    uint64_t interval_max_ns{0};
    uint32_t presented_frames{0};
    uint32_t present_failures{0};  // sink->Present returned false
    uint32_t bucket_60hz{0};       // ≤17ms
    uint32_t bucket_30hz{0};       // 18-33ms
    uint32_t bucket_20hz{0};       // 34-50ms
    uint32_t bucket_slow{0};       // 51-100ms
    uint32_t bucket_idle{0};       // >100ms
  };
  FrameProfile profile_{};
  FrameProfile session_totals_{};

  // Called from PresentTrampoline after sink->Present(); no-op when
  // IVI_SW_PROFILE is unset. Window log every 60 frames, session
  // summary emitted from dtor.
  void ProfilePresent(bool ok);

  uint32_t width_;
  uint32_t height_;
  std::unique_ptr<ISurfaceSink> sink_;

  // IVI_SW_STOP_AFTER_FRAMES=N: after N successful presents, raise
  // SIGTERM so the existing shutdown handler exits cleanly. Lets CI
  // bound runtime by frame count instead of `wall-clock`. 0 disables.
  // Sampled once at construction; the static check in PresentFrame
  // costs one atomic load on the rasterizer thread when disabled.
  uint64_t stop_after_frames_{0};
  std::atomic<uint64_t> presented_frames_{0};
  std::atomic<bool> stop_signaled_{false};
};
