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
  //
  // Also probes the native-fence-sync entry points, which are optional: a
  // display that imports dma-bufs but cannot wait on a producer's sync_file
  // still returns true here and simply reports has_native_fence_sync() false.
  bool Init(void* egl_display);

  [[nodiscard]] bool ready() const {
    return egl_display_ != nullptr && create_image_ != nullptr &&
           image_target_texture_ != nullptr;
  }

  // Whether this display can wait on a producer's sync_file, i.e. whether
  // WaitAcquireFence does anything. Independent of ready(): import and
  // explicit-sync acquire are separate driver capabilities, and this is what
  // backs IhsPvCapabilities::explicit_sync on an EGL backend (#513).
  [[nodiscard]] bool has_native_fence_sync() const {
    return egl_display_ != nullptr && create_sync_ != nullptr &&
           destroy_sync_ != nullptr && wait_sync_ != nullptr;
  }

  // Make the GL driver wait for @fence_fd (a producer acquire sync_file) before
  // any subsequent draw samples the import, by wrapping it in an
  // EGL_SYNC_NATIVE_FENCE_ANDROID and issuing a server-side eglWaitSyncKHR.
  // The GPU does the waiting; this call does not block.
  //
  // Returns true when the fence was handed to EGL, which CONSUMES @fence_fd
  // (EGL takes ownership and closes it) — the caller must not close it. Returns
  // false when there is no native fence sync or the sync could not be created,
  // leaving @fence_fd untouched and owned by the caller, which is the signal to
  // fall back to a CPU wait.
  //
  // GL context must be current, same as Import.
  [[nodiscard]] bool WaitAcquireFence(int fence_fd) const;

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
  // Native fence sync, for the explicit-sync acquire wait. Optional: null on a
  // display without EGL_KHR_fence_sync / EGL_ANDROID_native_fence_sync /
  // EGL_KHR_wait_sync, which leaves has_native_fence_sync() false.
  void* create_sync_{nullptr};   // PFNEGLCREATESYNCKHRPROC
  void* destroy_sync_{nullptr};  // PFNEGLDESTROYSYNCKHRPROC
  void* wait_sync_{nullptr};     // PFNEGLWAITSYNCKHRPROC
};
