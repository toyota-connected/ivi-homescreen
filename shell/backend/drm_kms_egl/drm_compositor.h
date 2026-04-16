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
#include <optional>
#include <unordered_map>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

#include <drm-cxx/planes/allocator.hpp>
#include <drm-cxx/planes/output.hpp>
#include <drm-cxx/planes/plane_registry.hpp>

#include <shell/platform/embedder/embedder.h>

#include "backend/wayland_egl/gl_caps.h"
#include "backend/wayland_egl/gl_compositor.h"

class DrmBackend;
class ICompositorSurface;

struct GbmBackingStore {
  gbm_bo* bo = nullptr;
  EGLImageKHR egl_image = EGL_NO_IMAGE_KHR;
  GLuint fbo = 0;
  GLuint color_tex = 0;
  GLuint depth_stencil_rb = 0;
  uint32_t drm_fb_id = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

// FlutterCompositor for the DRM/KMS backend with hardware-plane overlay
// support.
//
// Each Flutter-rendered layer is a GBM BO that can be scanned out directly
// on a KMS plane (via drm_fb_id) or composited into a composition buffer
// via GL when hardware planes are exhausted.
//
// PresentLayers builds a drm::planes::Output, runs the Allocator to assign
// layers to planes, GL-composites any overflow into a double-buffered
// composition GBM BO, and atomic-commits the result.
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
  bool InitPlaneAllocator();
  bool InitCompositionBuffers();
  void EnsureGlCapsProbed();
  void DestroyGbmStore(GbmBackingStore& store);
  bool CreateGbmStore(GbmBackingStore& store, uint32_t w, uint32_t h,
                      uint32_t format);
  uint32_t ImportBoAsFb(gbm_bo* bo);

  void CompositeLayerIntoFbo(GLuint target_fbo,
                             GLuint src_tex,
                             GLsizei src_w,
                             GLsizei src_h,
                             GLint dst_x,
                             GLint dst_y,
                             GLsizei dst_w,
                             GLsizei dst_h,
                             bool blend);

  bool WaitForPendingFlip();
  static void PageFlipHandler(int fd,
                              unsigned int sequence,
                              unsigned int tv_sec,
                              unsigned int tv_usec,
                              void* user_data);

  // GL fallback: composites all layers into FBO 0 and calls
  // DrmBackend::Present(). Used when the plane allocator isn't
  // available or when all layers need composition anyway.
  bool PresentViaGlFallback(const FlutterLayer** layers, size_t count);

  DrmBackend* backend_;

  // EGL image extensions.
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

  // Plane allocation.
  std::optional<drm::planes::PlaneRegistry> plane_registry_;
  std::unique_ptr<drm::planes::Allocator> allocator_;
  drm::planes::Layer comp_layer_;  // composition layer descriptor
  bool planes_available_{false};

  // Double-buffered composition buffer for layers that overflow HW planes.
  static constexpr int kNumCompBufs = 2;
  GbmBackingStore comp_bufs_[kNumCompBufs];
  int comp_idx_{0};
  bool comp_bufs_valid_{false};

  // Atomic-commit flip state (compositor owns its own flip lifecycle
  // when using the plane-allocator path).
  bool flip_pending_{false};
};
