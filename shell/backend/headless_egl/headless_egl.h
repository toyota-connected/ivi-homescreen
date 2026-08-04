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

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <atomic>
#include <cstdint>
#include <memory>

#include "asio/steady_timer.hpp"

#include "backend/backend.h"
#include "backend/software/nv12_consumer.h"
#include "backend/software/nv12_gl_packer.h"
#include "vsync/consumer_paced_vsync.h"
#include "vsync/ivsync_provider.h"

struct gbm_device;
struct gbm_surface;
struct gbm_bo;
class Engine;

// GPU headless encode backend: the Flutter engine renders each frame on the GPU
// (kOpenGL) into a real EGL window surface backed by a gbm_surface -- no
// display, no Wayland, no scanout. On present the frame is eglSwapBuffers'd,
// the just-presented gbm_bo is imported as a texture and packed RGBA->NV12 on
// the GPU straight into a dma-buf handed to an INv12Consumer (a file encoder or
// a WebRTC send). It is the zero-copy counterpart of the software backend's CPU
// EncoderSink: the GPU both rasterizes and colour-converts, no CPU touches the
// pixels.
//
// A real swap chain (gbm_surface + eglSwapBuffers) is used rather than a single
// reused FBO: rendering into an FBO made the engine composite incrementally
// against a buffer it wrongly assumed it could keep, leaving doubled widgets
// and motion trails. A window surface composites a full frame each present
// instead.
//
// Surfaceless GBM/EGL on the render node. Consumer selected by IVI_ENC_SINK,
// which is the bare consumer spec -- "file:<path>" (default "file:out.h264") or
// "webrtc:<host>:<port>".
class HeadlessEglBackend final : public Backend {
 public:
  HeadlessEglBackend(uint32_t initial_width,
                     uint32_t initial_height,
                     std::unique_ptr<INv12Consumer> consumer);
  ~HeadlessEglBackend() override;

  void Resize(size_t index,
              Engine* flutter_engine,
              int32_t width,
              int32_t height) override;
  void CreateSurface(size_t index,
                     wl_surface* surface,
                     int32_t width,
                     int32_t height) override;

  bool TextureMakeCurrent() override;
  bool TextureClearCurrent() override;

  FlutterRendererConfig GetRenderConfig() override;
  FlutterCompositor GetCompositorConfig() override;

  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }

  // Called from the engine's OpenGL renderer config trampolines (raster
  // thread).
  bool MakeCurrent();
  bool ClearCurrent();
  bool MakeResourceCurrent();
  // Frame done: swap the window surface, import the presented buffer, pack it.
  bool Present(const FlutterPresentInfo* info);

  // Synthetic vsync: without a display there is no vblank, so the engine would
  // render wall-clock (~60fps) and the packer would drop most frames. Instead
  // pace the engine to the encode rate with a timer (IVI_ENC_MAX_FPS, default
  // 30; IVI_HEADLESS_VSYNC=0 disables and falls back to wall-clock). The engine
  // stays demand-driven -- an idle UI still schedules nothing.
  [[nodiscard]] VsyncCallback GetVsyncCallback() const override;
  void SetEngineHandle(FLUTTER_API_SYMBOL(FlutterEngine) engine) override;
  void SetPlatformTaskRunner(TaskRunner* runner) override;
  void SetVsyncParked(const bool parked) override { vsync_.SetParked(parked); }

  void StopVsyncMonitor() override;

 private:
  bool InitEgl(
      const char* render_node);  // display, config, contexts, swapchain
  bool
  InitRenderTarget();  // packer + import texture; GL context must be current
  void Teardown();

  // The engine's vsync_callback trampoline: parks the baton with vsync_.
  static void VsyncTrampoline(void* user_data, intptr_t baton);
  // Start the timer once both the engine handle and the runner are wired.
  void StartVsyncIfReady();
  // Schedule the next tick; the handler runs on the runner's strand.
  void ArmVsyncTimer();

  uint32_t width_{0};
  uint32_t height_{0};
  uint64_t frame_index_{0};

  int render_fd_{-1};
  gbm_device* gbm_{nullptr};
  gbm_surface* gbm_surface_{nullptr};
  uint32_t gbm_format_{0};  // DRM fourcc of the swap-chain buffers

  EGLDisplay dpy_{EGL_NO_DISPLAY};
  EGLConfig config_{nullptr};
  EGLSurface egl_surface_{EGL_NO_SURFACE};
  EGLContext ctx_{EGL_NO_CONTEXT};
  EGLContext resource_ctx_{EGL_NO_CONTEXT};

  GLuint import_tex_{0};  // GL_TEXTURE_2D the presented bo is imported into
  gbm_bo* locked_bo_{
      nullptr};  // front buffer held until the next swap frees it

  bool target_ready_{false};

  // Synthetic-vsync pacing. vsync_ holds the baton machinery. Two drivers of
  // it: the default free-running steady_timer (fixed target rate), or -- when
  // paced_ (IVI_HEADLESS_PACED=1) -- a ConsumerPacedVsyncSource that only
  // delivers a baton when the encode ring has a free slot (backpressure), with
  // the same rate as a ceiling. The pacer is the backend-agnostic path a future
  // Vulkan encoder reuses.
  ivi::IVsyncProvider vsync_;
  FLUTTER_API_SYMBOL(FlutterEngine) engine_handle_ { nullptr };
  TaskRunner* platform_task_runner_{nullptr};
  std::unique_ptr<asio::steady_timer> vsync_timer_;
  std::atomic<bool> vsync_running_{false};
  uint32_t vsync_period_ns_{0};  // 0 = disabled (wall-clock scheduler)
  bool paced_{false};            // consumer-paced vsync instead of the timer
  bool free_run_{false};         // paced source in ceiling-only (detached) mode
  std::unique_ptr<ivi::ConsumerPacedVsyncSource> pacer_;

  std::unique_ptr<INv12Consumer> consumer_;
  Nv12GlPacker packer_;
};
