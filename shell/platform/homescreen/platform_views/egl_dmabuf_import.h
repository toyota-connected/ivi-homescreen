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

#include "ihs/platform_view.h"

// Imports a plugin-produced dma-buf (IhsFrame) into a GL_TEXTURE_2D on the
// compositor's own GL/EGL context, so an ihs_pv platform view's output is a
// first-class GL texture the EGL backends composite via
// ICompositorSurface::GetGlTextureName(). The EGL counterpart to
// DmabufVulkanImporter: the imported texture aliases the dma-buf's memory (no
// pixel copy) via EGL_EXT_image_dma_buf_import + glEGLImageTargetTexture2DOES,
// created once per ring buffer (buffer_id).
//
// A GLES producer (e.g. MapLibre's GL backend) and a Vulkan producer submit the
// SAME dma-buf ABI; only which importer the host runs differs — this one on an
// EGL backend, DmabufVulkanImporter on a Vulkan backend.
//
// GL affinity: Import/Destroy touch GL objects and must run with the backend's
// GL context current (the raster thread, during present) — the same thread the
// backend composites GetGlTextureName() on.
class EglDmabufImporter {
 public:
  struct ImportedTexture {
    unsigned int texture{0};   // GLuint, 0 = unset
    void* egl_image{nullptr};  // EGLImageKHR
    uint32_t width{0};
    uint32_t height{0};
    // Which target the texture is bound to. A YUV image — planar (NV12, ...) or
    // packed (YUYV/UYVY) — must be GL_TEXTURE_EXTERNAL_OES so the driver does
    // the YUV->RGB conversion; sampling one as GL_TEXTURE_2D yields raw/luma
    // data. Packed RGB stays on GL_TEXTURE_2D, which every GLES context
    // supports.
    bool external{false};
  };

  EglDmabufImporter() = default;

  // Resolve the EGL/GL import entry points against @egl_display (the EGLDisplay
  // from Backend::GetEglContext). Returns false when the driver lacks
  // EGL_EXT_image_dma_buf_import / glEGLImageTargetTexture2DOES (the plugin
  // then falls back to the software floor). Call once before Import; no GL
  // context needs to be current for Init.
  bool Init(void* egl_display);

  [[nodiscard]] bool ready() const {
    return egl_display_ != nullptr && create_image_ != nullptr &&
           image_target_texture_ != nullptr;
  }

  // Import @frame's dma-buf planes into a texture in @out. On success the
  // texture owns the import; @frame.plane_fd[*] are consumed (EGL dup's them,
  // so they are closed here). On failure the fds are left for the caller to
  // close.
  //
  // YUV formats — planar (NV12, ...) or packed (YUYV/UYVY) — are bound as
  // GL_TEXTURE_EXTERNAL_OES, which is what applies the color conversion; packed
  // RGB stays GL_TEXTURE_2D. ImportedTexture::external says which, and the
  // caller must sample with a matching sampler. GL context must be current.
  bool Import(const IhsFrame& frame, ImportedTexture* out) const;

  void Destroy(ImportedTexture* out) const;

 private:
  void* egl_display_{nullptr};  // EGLDisplay
  // Resolved via eglGetProcAddress; typed as void* to keep EGL/GL headers out
  // of this public header (the .cc casts to the PFN types).
  void* create_image_{nullptr};          // PFNEGLCREATEIMAGEKHRPROC
  void* destroy_image_{nullptr};         // PFNEGLDESTROYIMAGEKHRPROC
  void* image_target_texture_{nullptr};  // PFNGLEGLIMAGETARGETTEXTURE2DOESPROC
};
