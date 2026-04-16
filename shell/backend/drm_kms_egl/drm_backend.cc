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

#include "backend/drm_kms_egl/drm_backend.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <utility>

#include "backend/drm_kms_egl/drm_compositor.h"
#include "backend/gl_process_resolver.h"
#include "engine.h"
#include "logging.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"

#if BUILD_COMPOSITOR
#include "view/compositor_surface_interface.h"
#endif

namespace {

constexpr std::array<EGLint, 15> kDrmEglConfigAttribs = {{
    EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_BLUE_SIZE, 8,
    EGL_ALPHA_SIZE, 0,
    EGL_DEPTH_SIZE, 24,
    EGL_NONE,
}};

constexpr std::array<EGLint, 3> kEsContextAttribs = {{
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE,
}};

DrmBackend* BackendFromState(void* user_data) {
  const auto state = static_cast<FlutterDesktopEngineState*>(user_data);
  return reinterpret_cast<DrmBackend*>(
      state->view_controller->engine->GetBackend());
}

}  // namespace

std::unique_ptr<DrmBackend> DrmBackend::Create(const DrmConfig& cfg) {
  std::unique_ptr<DrmBackend> backend(new DrmBackend(cfg));
  if (!backend->InitDrm() || !backend->InitGbm() || !backend->InitEgl()) {
    return nullptr;
  }
#if BUILD_COMPOSITOR
  backend->compositor_ = std::make_unique<DrmCompositor>(backend.get());
#endif
  return backend;
}

DrmBackend::DrmBackend(DrmConfig cfg) : cfg_(std::move(cfg)) {}

DrmBackend::~DrmBackend() {
  // Let any in-flight page flip land so we don't free a BO still being
  // scanned out.
  WaitForPendingFlip();

#if BUILD_COMPOSITOR
  // Release compositor GL resources while the context is still current.
  if (egl_display_ != EGL_NO_DISPLAY && egl_context_ != EGL_NO_CONTEXT) {
    eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
  }
  compositor_.reset();
#endif

  if (egl_display_ != EGL_NO_DISPLAY) {
    eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    if (egl_surface_ != EGL_NO_SURFACE) {
      eglDestroySurface(egl_display_, egl_surface_);
    }
    if (egl_texture_context_ != EGL_NO_CONTEXT) {
      eglDestroyContext(egl_display_, egl_texture_context_);
    }
    if (egl_resource_context_ != EGL_NO_CONTEXT) {
      eglDestroyContext(egl_display_, egl_resource_context_);
    }
    if (egl_context_ != EGL_NO_CONTEXT) {
      eglDestroyContext(egl_display_, egl_context_);
    }
    eglTerminate(egl_display_);
  }

  if (drm_fd_ >= 0 && saved_crtc_) {
    drmModeSetCrtc(drm_fd_, saved_crtc_->crtc_id, saved_crtc_->buffer_id,
                   saved_crtc_->x, saved_crtc_->y, &connector_id_, 1,
                   &saved_crtc_->mode);
    drmModeFreeCrtc(saved_crtc_);
  }

  if (pending_fb_ != 0) {
    drmModeRmFB(drm_fd_, pending_fb_);
  }
  if (pending_bo_) {
    gbm_surface_release_buffer(gbm_surface_, pending_bo_);
  }
  if (current_fb_ != 0) {
    drmModeRmFB(drm_fd_, current_fb_);
  }
  if (current_bo_) {
    gbm_surface_release_buffer(gbm_surface_, current_bo_);
  }
  if (gbm_surface_) {
    gbm_surface_destroy(gbm_surface_);
  }
  if (gbm_device_) {
    gbm_device_destroy(gbm_device_);
  }
  if (drm_fd_ >= 0) {
    close(drm_fd_);
  }
}

bool DrmBackend::InitDrm() {
  drm_fd_ = open(cfg_.drm_device.c_str(), O_RDWR | O_CLOEXEC);
  if (drm_fd_ < 0) {
    spdlog::error("[DrmBackend] open({}): {}", cfg_.drm_device,
                  std::strerror(errno));
    return false;
  }

  if (drmSetClientCap(drm_fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
    spdlog::warn("[DrmBackend] DRM_CLIENT_CAP_UNIVERSAL_PLANES unsupported");
  }
  if (drmSetClientCap(drm_fd_, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
    spdlog::warn("[DrmBackend] DRM_CLIENT_CAP_ATOMIC unsupported");
  }

  drmModeRes* res = drmModeGetResources(drm_fd_);
  if (!res) {
    spdlog::error("[DrmBackend] drmModeGetResources failed: {}",
                  std::strerror(errno));
    return false;
  }

  drmModeConnector* connector = nullptr;
  for (int i = 0; i < res->count_connectors; ++i) {
    connector = drmModeGetConnector(drm_fd_, res->connectors[i]);
    if (connector && connector->connection == DRM_MODE_CONNECTED &&
        connector->count_modes > 0) {
      break;
    }
    drmModeFreeConnector(connector);
    connector = nullptr;
  }

  if (!connector) {
    spdlog::error("[DrmBackend] no connected connector found");
    drmModeFreeResources(res);
    return false;
  }
  connector_id_ = connector->connector_id;

  for (int i = 0; i < connector->count_modes; ++i) {
    const auto& m = connector->modes[i];
    if (m.hdisplay == cfg_.width && m.vdisplay == cfg_.height) {
      mode_ = m;
      break;
    }
    if (m.type & DRM_MODE_TYPE_PREFERRED) {
      mode_ = m;
    }
  }
  if (mode_.clock == 0) {
    mode_ = connector->modes[0];
  }

  drmModeEncoder* enc = nullptr;
  if (connector->encoder_id) {
    enc = drmModeGetEncoder(drm_fd_, connector->encoder_id);
  }
  if (enc && enc->crtc_id) {
    crtc_id_ = enc->crtc_id;
  } else {
    for (int e = 0; e < connector->count_encoders && !crtc_id_; ++e) {
      drmModeEncoder* candidate =
          drmModeGetEncoder(drm_fd_, connector->encoders[e]);
      if (!candidate) {
        continue;
      }
      for (int c = 0; c < res->count_crtcs; ++c) {
        if (candidate->possible_crtcs & (1u << c)) {
          crtc_id_ = res->crtcs[c];
          crtc_index_ = static_cast<uint32_t>(c);
          break;
        }
      }
      drmModeFreeEncoder(candidate);
    }
  }
  if (enc) {
    for (int c = 0; c < res->count_crtcs; ++c) {
      if (res->crtcs[c] == crtc_id_) {
        crtc_index_ = static_cast<uint32_t>(c);
        break;
      }
    }
    drmModeFreeEncoder(enc);
  }

  if (!crtc_id_) {
    spdlog::error("[DrmBackend] no CRTC available for connector {}",
                  connector_id_);
    drmModeFreeConnector(connector);
    drmModeFreeResources(res);
    return false;
  }

  saved_crtc_ = drmModeGetCrtc(drm_fd_, crtc_id_);

  spdlog::info("[DrmBackend] connector={} crtc={} mode={}x{}@{}Hz",
               connector_id_, crtc_id_, mode_.hdisplay, mode_.vdisplay,
               mode_.vrefresh);

  drmModeFreeConnector(connector);
  drmModeFreeResources(res);
  return true;
}

bool DrmBackend::InitGbm() {
  gbm_device_ = gbm_create_device(drm_fd_);
  if (!gbm_device_) {
    spdlog::error("[DrmBackend] gbm_create_device failed");
    return false;
  }

  gbm_surface_ = gbm_surface_create(
      gbm_device_, mode_.hdisplay, mode_.vdisplay, GBM_FORMAT_XRGB8888,
      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
  if (!gbm_surface_) {
    spdlog::error("[DrmBackend] gbm_surface_create failed");
    return false;
  }
  return true;
}

bool DrmBackend::InitEgl() {
  auto get_platform_display =
      reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
          eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (get_platform_display) {
    egl_display_ =
        get_platform_display(EGL_PLATFORM_GBM_KHR, gbm_device_, nullptr);
  } else {
    egl_display_ = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(
        gbm_device_));
  }
  if (egl_display_ == EGL_NO_DISPLAY) {
    spdlog::error("[DrmBackend] eglGetPlatformDisplay failed");
    return false;
  }

  EGLint major = 0;
  EGLint minor = 0;
  if (!eglInitialize(egl_display_, &major, &minor)) {
    spdlog::error("[DrmBackend] eglInitialize failed: 0x{:x}", eglGetError());
    return false;
  }
  SPDLOG_DEBUG("[DrmBackend] EGL {}.{}", major, minor);

  if (!eglBindAPI(EGL_OPENGL_ES_API)) {
    spdlog::error("[DrmBackend] eglBindAPI failed");
    return false;
  }

  EGLint n = 0;
  if (!eglChooseConfig(egl_display_, kDrmEglConfigAttribs.data(), &egl_config_,
                       1, &n) ||
      n < 1) {
    spdlog::error("[DrmBackend] eglChooseConfig failed");
    return false;
  }

  egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT,
                                  kEsContextAttribs.data());
  if (egl_context_ == EGL_NO_CONTEXT) {
    spdlog::error("[DrmBackend] eglCreateContext failed: 0x{:x}",
                  eglGetError());
    return false;
  }
  egl_resource_context_ = eglCreateContext(
      egl_display_, egl_config_, egl_context_, kEsContextAttribs.data());
  egl_texture_context_ = eglCreateContext(
      egl_display_, egl_config_, egl_context_, kEsContextAttribs.data());

  egl_surface_ = eglCreateWindowSurface(
      egl_display_, egl_config_,
      reinterpret_cast<EGLNativeWindowType>(gbm_surface_), nullptr);
  if (egl_surface_ == EGL_NO_SURFACE) {
    spdlog::error("[DrmBackend] eglCreateWindowSurface failed: 0x{:x}",
                  eglGetError());
    return false;
  }
  return true;
}

uint32_t DrmBackend::AddFb(gbm_bo* bo) {
  const uint32_t width = gbm_bo_get_width(bo);
  const uint32_t height = gbm_bo_get_height(bo);
  const uint32_t stride = gbm_bo_get_stride(bo);
  const uint32_t handle = gbm_bo_get_handle(bo).u32;
  uint32_t fb_id = 0;
  if (drmModeAddFB(drm_fd_, width, height, 24, 32, stride, handle, &fb_id) !=
      0) {
    spdlog::error("[DrmBackend] drmModeAddFB: {}", std::strerror(errno));
    return 0;
  }
  return fb_id;
}

bool DrmBackend::SetInitialMode() {
  if (!current_bo_ || current_fb_ == 0) {
    return false;
  }
  if (drmModeSetCrtc(drm_fd_, crtc_id_, current_fb_, 0, 0, &connector_id_, 1,
                     &mode_) != 0) {
    spdlog::error("[DrmBackend] drmModeSetCrtc: {}", std::strerror(errno));
    return false;
  }
  mode_set_ = true;
  return true;
}

bool DrmBackend::MakeCurrent() {
  return eglMakeCurrent(egl_display_, egl_surface_, egl_surface_,
                        egl_context_) == EGL_TRUE;
}

bool DrmBackend::ClearCurrent() {
  return eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        EGL_NO_CONTEXT) == EGL_TRUE;
}

bool DrmBackend::MakeResourceCurrent() {
  return eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        egl_resource_context_) == EGL_TRUE;
}

bool DrmBackend::TextureMakeCurrent() {
  return eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        egl_texture_context_) == EGL_TRUE;
}

bool DrmBackend::TextureClearCurrent() {
  return eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        EGL_NO_CONTEXT) == EGL_TRUE;
}

void DrmBackend::PageFlipHandler(int /*fd*/,
                                 unsigned int /*sequence*/,
                                 unsigned int /*tv_sec*/,
                                 unsigned int /*tv_usec*/,
                                 void* user_data) {
  auto* self = static_cast<DrmBackend*>(user_data);
  // The page flip just promoted pending → scanout. What was scanned out
  // before is now safe to release.
  if (self->current_fb_ != 0) {
    drmModeRmFB(self->drm_fd_, self->current_fb_);
  }
  if (self->current_bo_) {
    gbm_surface_release_buffer(self->gbm_surface_, self->current_bo_);
  }
  self->current_bo_ = self->pending_bo_;
  self->current_fb_ = self->pending_fb_;
  self->pending_bo_ = nullptr;
  self->pending_fb_ = 0;
  self->flip_pending_ = false;
}

bool DrmBackend::WaitForPendingFlip() {
  if (!flip_pending_ || drm_fd_ < 0) {
    return true;
  }

  drmEventContext ctx{};
  ctx.version = 2;
  ctx.page_flip_handler = &DrmBackend::PageFlipHandler;

  while (flip_pending_) {
    pollfd pfd{};
    pfd.fd = drm_fd_;
    pfd.events = POLLIN;
    const int r = poll(&pfd, 1, -1);
    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      spdlog::error("[DrmBackend] poll: {}", std::strerror(errno));
      return false;
    }
    if (pfd.revents & POLLIN) {
      drmHandleEvent(drm_fd_, &ctx);
    }
  }
  return true;
}

bool DrmBackend::Present() {
  // Finish the previous flip before issuing a new one. The kernel rejects a
  // second queued flip while one is still in flight.
  if (!WaitForPendingFlip()) {
    return false;
  }

  if (!eglSwapBuffers(egl_display_, egl_surface_)) {
    spdlog::error("[DrmBackend] eglSwapBuffers: 0x{:x}", eglGetError());
    return false;
  }

  gbm_bo* next_bo = gbm_surface_lock_front_buffer(gbm_surface_);
  if (!next_bo) {
    spdlog::error("[DrmBackend] gbm_surface_lock_front_buffer failed");
    return false;
  }

  const uint32_t next_fb = AddFb(next_bo);
  if (next_fb == 0) {
    gbm_surface_release_buffer(gbm_surface_, next_bo);
    return false;
  }

  if (!mode_set_) {
    // First frame: drive the mode set synchronously. current_* hold the
    // active scanout BO/FB until the next successful flip.
    current_bo_ = next_bo;
    current_fb_ = next_fb;
    return SetInitialMode();
  }

  if (drmModePageFlip(drm_fd_, crtc_id_, next_fb, DRM_MODE_PAGE_FLIP_EVENT,
                      this) != 0) {
    spdlog::warn("[DrmBackend] drmModePageFlip: {}", std::strerror(errno));
    drmModeRmFB(drm_fd_, next_fb);
    gbm_surface_release_buffer(gbm_surface_, next_bo);
    return false;
  }

  // The kernel now owns next_bo/next_fb until the page-flip-complete event
  // fires. current_bo_/current_fb_ remain the live scanout until then.
  pending_bo_ = next_bo;
  pending_fb_ = next_fb;
  flip_pending_ = true;
  return true;
}

void DrmBackend::Resize(size_t /*index*/,
                        Engine* /*engine*/,
                        int32_t /*w*/,
                        int32_t /*h*/) {
  // DRM/KMS mode is fixed at the connector; engine is driven by the initial
  // mode resolution. Runtime mode switches are not supported in this phase.
}

FlutterRendererConfig DrmBackend::GetRenderConfig() {
  FlutterRendererConfig config{};
  config.type = kOpenGL;
  config.open_gl.struct_size = sizeof(FlutterOpenGLRendererConfig);

  config.open_gl.make_current = [](void* user_data) -> bool {
    return BackendFromState(user_data)->MakeCurrent();
  };
  config.open_gl.clear_current = [](void* user_data) -> bool {
    return BackendFromState(user_data)->ClearCurrent();
  };
  config.open_gl.present = [](void* user_data) -> bool {
    return BackendFromState(user_data)->Present();
  };
  config.open_gl.fbo_callback = [](void* /*user_data*/) -> uint32_t {
    return 0;  // window FBO
  };
  config.open_gl.make_resource_current = [](void* user_data) -> bool {
    return BackendFromState(user_data)->MakeResourceCurrent();
  };
  config.open_gl.fbo_reset_after_present = false;
  config.open_gl.gl_proc_resolver = [](void* /*userdata*/,
                                       const char* name) -> void* {
    return GlProcessResolver::GetInstance().process_resolver(name);
  };
  return config;
}

FlutterCompositor DrmBackend::GetCompositorConfig() {
  FlutterCompositor compositor{};
  compositor.struct_size = sizeof(FlutterCompositor);
  compositor.user_data = this;

#if BUILD_COMPOSITOR
  // The engine reuses backing stores across frames when sizes match;
  // allow caching since our EglFboBackingStore is identity-keyed.
  compositor.avoid_backing_store_cache = false;
  compositor.create_backing_store_callback =
      [](const FlutterBackingStoreConfig* config, FlutterBackingStore* out,
         void* user_data) -> bool {
    return static_cast<DrmBackend*>(user_data)->compositor_->CreateBackingStore(
        config, out);
  };
  compositor.collect_backing_store_callback =
      [](const FlutterBackingStore* store, void* user_data) -> bool {
    return static_cast<DrmBackend*>(user_data)
        ->compositor_->CollectBackingStore(store);
  };
  compositor.present_layers_callback =
      [](const FlutterLayer** layers, size_t count, void* user_data) -> bool {
    return static_cast<DrmBackend*>(user_data)->compositor_->PresentLayers(
        layers, count);
  };
#else
  compositor.avoid_backing_store_cache = true;
#endif
  return compositor;
}

#if BUILD_COMPOSITOR
void DrmBackend::RegisterCompositorSurface(
    FlutterPlatformViewIdentifier id,
    std::shared_ptr<ICompositorSurface> surface) {
  if (compositor_) {
    compositor_->RegisterSurface(id, std::move(surface));
  }
}

void DrmBackend::UnregisterCompositorSurface(
    FlutterPlatformViewIdentifier id) {
  if (compositor_) {
    compositor_->UnregisterSurface(id);
  }
}

void DrmBackend::ResizeCompositorSurface(FlutterPlatformViewIdentifier id,
                                         int32_t width,
                                         int32_t height) {
  if (compositor_) {
    compositor_->ResizeSurface(id, width, height);
  }
}
#endif

bool DrmBackend::GetEglContext(BackendEglContext* out) const {
  if (!out) {
    return false;
  }
  out->display = egl_display_;
  out->config = egl_config_;
  out->share_context = egl_context_;
  return true;
}