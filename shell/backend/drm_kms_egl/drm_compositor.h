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

#include <GLES2/gl2.h>

#include <shell/platform/embedder/embedder.h>

#include "backend/wayland_egl/egl_backing_store.h"
#include "backend/wayland_egl/gl_caps.h"
#include "backend/wayland_egl/gl_compositor.h"

class DrmBackend;
class ICompositorSurface;

// FlutterCompositor callbacks for the DRM/KMS backend.
//
// Each Flutter-rendered layer is a DIY FBO (EglFboBackingStore); on
// PresentLayers the compositor clears the default framebuffer, composites
// every layer (backing store or registered platform-view texture) in
// engine order, then hands off to DrmBackend::Present which performs the
// eglSwapBuffers / gbm_surface_lock / drmModePageFlip sequence.
//
// This implementation composites everything into the primary framebuffer;
// DRM overlay planes are not used. Swapping each layer onto its own KMS
// plane via drm-cxx's Allocator is a follow-up optimisation that does not
// change the FlutterCompositor contract.
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

  // Platform-view surface registry. Register/Unregister fire on the
  // platform thread via FlutterView; PresentLayers reads on the rasterizer
  // thread. surfaces_mu_ covers only the map — the mutex is never held
  // across OnPresent().
  void RegisterSurface(FlutterPlatformViewIdentifier id,
                       std::shared_ptr<ICompositorSurface> surface);
  void UnregisterSurface(FlutterPlatformViewIdentifier id);
  void ResizeSurface(FlutterPlatformViewIdentifier id,
                     int32_t width,
                     int32_t height);

 private:
  struct StoreBaton {
    DrmCompositor* owner;
    EglFboBackingStore* store;
  };

  void EnsureGlCapsProbed();
  void CompositeBackingStore(const FlutterBackingStore* store,
                             GLint dst_x,
                             GLint dst_y,
                             GLsizei dst_w,
                             GLsizei dst_h,
                             bool blend);
  void CompositePlatformView(const FlutterLayer* layer, bool blend);

  DrmBackend* backend_;  // not owned

  // GL helpers are lazy-initialised on the rasterizer thread with the
  // engine's context current (first CreateBackingStore / PresentLayers).
  GlCaps gl_caps_{};
  bool gl_caps_probed_{false};
  std::unique_ptr<GlCompositor> gl_compositor_;

  // Live backing stores keyed on the StoreBaton pointer we hand to the
  // engine. CreateBackingStore / CollectBackingStore are both on the
  // rasterizer thread, so no lock is needed here.
  std::unordered_map<StoreBaton*, std::unique_ptr<EglFboBackingStore>> stores_;

  mutable std::mutex surfaces_mu_;
  std::unordered_map<FlutterPlatformViewIdentifier,
                     std::shared_ptr<ICompositorSurface>>
      surfaces_;

  // Scratch FBO for compositing a platform-view texture via
  // glBlitFramebuffer when the opaque fast path is available. Lazily
  // created; lifetime tied to this object's GL context.
  GLuint texture_blit_fbo_{0};
};
