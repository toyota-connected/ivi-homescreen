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

#include <memory>
#include <string>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <shell/platform/embedder/embedder.h>

#include "backend/backend.h"

class DrmCompositor;

struct DrmConfig {
  std::string drm_device;
  uint32_t width;
  uint32_t height;
  bool debug_backend{false};
};

class DrmBackend : public Backend {
 public:
  static std::unique_ptr<DrmBackend> Create(const DrmConfig& cfg);
  ~DrmBackend() override;

  DrmBackend(const DrmBackend&) = delete;
  DrmBackend& operator=(const DrmBackend&) = delete;

  void Resize(size_t index, Engine* engine, int32_t w, int32_t h) override;
  void CreateSurface(size_t, struct wl_surface*, int32_t, int32_t) override {}
  bool TextureMakeCurrent() override;
  bool TextureClearCurrent() override;
  FlutterRendererConfig GetRenderConfig() override;
  FlutterCompositor GetCompositorConfig() override;
  bool GetEglContext(BackendEglContext* out) const override;

#if BUILD_COMPOSITOR
  void RegisterCompositorSurface(
      FlutterPlatformViewIdentifier id,
      std::shared_ptr<ICompositorSurface> surface) override;
  void UnregisterCompositorSurface(
      FlutterPlatformViewIdentifier id) override;
  void ResizeCompositorSurface(FlutterPlatformViewIdentifier id,
                               int32_t width,
                               int32_t height) override;
#endif

  bool MakeCurrent();
  bool ClearCurrent();
  bool MakeResourceCurrent();
  bool Present();

  [[nodiscard]] uint32_t width() const { return mode_.hdisplay; }
  [[nodiscard]] uint32_t height() const { return mode_.vdisplay; }
  [[nodiscard]] EGLDisplay egl_display() const { return egl_display_; }

 private:
  explicit DrmBackend(DrmConfig cfg);
  bool InitDrm();
  bool InitGbm();
  bool InitEgl();
  bool SetInitialMode();
  uint32_t AddFb(gbm_bo* bo);
  bool WaitForPendingFlip();
  static void PageFlipHandler(int fd,
                              unsigned int sequence,
                              unsigned int tv_sec,
                              unsigned int tv_usec,
                              void* user_data);

  DrmConfig cfg_;

  // DRM
  int drm_fd_ = -1;
  uint32_t connector_id_ = 0;
  uint32_t crtc_id_ = 0;
  uint32_t crtc_index_ = 0;
  drmModeModeInfo mode_{};
  drmModeCrtc* saved_crtc_ = nullptr;

  // GBM. `current_bo_`/`current_fb_` scan out right now. `pending_bo_`/
  // `pending_fb_` were handed to the kernel via drmModePageFlip and are
  // released when the page-flip-complete event arrives.
  gbm_device* gbm_device_ = nullptr;
  gbm_surface* gbm_surface_ = nullptr;
  gbm_bo* current_bo_ = nullptr;
  uint32_t current_fb_ = 0;
  gbm_bo* pending_bo_ = nullptr;
  uint32_t pending_fb_ = 0;
  bool flip_pending_ = false;

  // EGL
  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLConfig egl_config_ = nullptr;
  EGLContext egl_context_ = EGL_NO_CONTEXT;
  EGLContext egl_resource_context_ = EGL_NO_CONTEXT;
  EGLContext egl_texture_context_ = EGL_NO_CONTEXT;
  EGLSurface egl_surface_ = EGL_NO_SURFACE;

  bool mode_set_ = false;

#if BUILD_COMPOSITOR
  // Compositor lives as a member so its destructor runs while the EGL
  // context is still current (DrmBackend::~DrmBackend resets it before
  // tearing EGL down).
  std::unique_ptr<DrmCompositor> compositor_;
#endif
};
