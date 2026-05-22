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

  // Vsync wiring is gated on the sink advertising a real vblank source
  // (DRM dumb buffer is the only sink that currently does). Sinks
  // without a vblank source return false from SupportsVsync(),
  // GetVsyncCallback() returns nullptr, and Flutter falls back to its
  // internal wall-clock scheduler.
  [[nodiscard]] VsyncCallback GetVsyncCallback() const override {
    return (sink_ && sink_->SupportsVsync()) ? &VsyncTrampoline : nullptr;
  }
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

  uint32_t width_;
  uint32_t height_;
  std::unique_ptr<ISurfaceSink> sink_;

  // IVI_SW_STOP_AFTER_FRAMES=N: after N successful presents, raise
  // SIGTERM so the existing shutdown handler exits cleanly. Lets CI
  // bound runtime by frame count instead of wall-clock. 0 disables.
  // Sampled once at construction; the static check in PresentFrame
  // costs one atomic load on the rasterizer thread when disabled.
  uint64_t stop_after_frames_{0};
  std::atomic<uint64_t> presented_frames_{0};
  std::atomic<bool> stop_signaled_{false};
};
