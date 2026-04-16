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
#include <memory>
#include <mutex>
#include <unordered_map>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

#include <shell/platform/embedder/embedder.h>

#include "backend/wayland_egl/gl_caps.h"
#include "backend/wayland_egl/gl_compositor.h"

class DrmBackend;
class ICompositorSurface;

// GBM-backed backing store. Flutter renders into the GL FBO; the BO can
// be scanned out directly on a KMS plane via drm_fb_id, or composited
// into a composition buffer via GL when hardware planes are exhausted.
struct GbmBackingStore {
  gbm_bo* bo = nullptr;
  EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
  GLuint fbo = 0;
  GLuint color_tex = 0;        // GL_TEXTURE_2D backed by the EGL image
  GLuint depth_stencil_rb = 0;
  uint32_t drm_fb_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// FlutterCompositor callbacks for the DRM/KMS backend.
//
// Each Flutter-rendered layer is a GBM BO wrapped in an EGL image + GL FBO.
// The BO is simultaneously scanout-capable (KMS framebuffer) and renderable
// (GL FBO via EGLImageTargetTexture2DOES).
//
// PresentLayers currently composites all layers into the primary framebuffer
// via GL, then hands off to DrmBackend::Present for the page flip. The
// plane-allocator path (step 3) will replace this with direct per-layer
// plane assignment for layers the hardware can scan out.
class DrmCompositor {
 public:
  explicit DrmCompositor(DrmBackend* backend);
  ~DrmCompositor();

  DrmCompositor(const DrmCompositor&) = delete;
  DrmCompositor& operator=(const DrmCompositor&) = delete;

  bool CreateBackingStore(const FlutterBackingStoreConfig* config,
                          FlutterBackingStore* out);
  bool CollectBackingStore(const FlutterBackingStore* store);
  bool PresentLayers(const FlutterLayer** layers, size_t layer_count);

  void RegisterSurface(FlutterPlatformViewIdentifier id,
                       std::shared_ptr<ICompositorSurface> surface);
  void UnregisterSurface(FlutterPlatformViewIdentifier id);
  void ResizeSurface(FlutterPlatformViewIdentifier id,
                     int32_t width,
                     int32_t height);

 private:
  struct StoreBaton {
    DrmCompositor* owner;
    GbmBackingStore* store;
  };

  bool InitEglExtensions();
  void EnsureGlCapsProbed();
  void DestroyGbmStore(GbmBackingStore& store);
  uint32_t ImportBoAsFb(gbm_bo* bo);

  void CompositeBackingStore(const GbmBackingStore* store,
                             GLint dst_x,
                             GLint dst_y,
                             GLsizei dst_w,
                             GLsizei dst_h,
                             bool blend);
  void CompositePlatformView(const FlutterLayer* layer, bool blend);

  DrmBackend* backend_;

  // EGL image extension pointers (resolved once).
  PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR_ = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR_ = nullptr;
  PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES_ = nullptr;

  GlCaps gl_caps_{};
  bool gl_caps_probed_{false};
  std::unique_ptr<GlCompositor> gl_compositor_;

  std::unordered_map<StoreBaton*, std::unique_ptr<GbmBackingStore>> stores_;

  mutable std::mutex surfaces_mu_;
  std::unordered_map<FlutterPlatformViewIdentifier,
                     std::shared_ptr<ICompositorSurface>>
      surfaces_;

  GLuint texture_blit_fbo_{0};
};
