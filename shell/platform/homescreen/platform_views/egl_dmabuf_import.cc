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

#include "egl_dmabuf_import.h"

#include <unistd.h>

#include <array>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <cstring>
#include <string_view>

#include <drm_fourcc.h>

#include "backend/gl_extensions.h"

#include "logging/logging.h"

namespace {

// EGL dma-buf plane attribute names, indexed by plane (0..3). EGL spells them
// out per plane rather than offering an indexed form.
struct PlaneAttribs {
  EGLint fd;
  EGLint offset;
  EGLint pitch;
  EGLint modifier_lo;
  EGLint modifier_hi;
};
constexpr std::array<PlaneAttribs, 4> kPlaneAttribs{{
    {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT,
     EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
     EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
    {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
     EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
     EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
    {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT,
     EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
     EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
    {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT,
     EGL_DMA_BUF_PLANE3_PITCH_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
     EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT},
}};

// Queried once with a context current, which Import guarantees.
bool HasExternalImage() {
  static const bool supported = ihs::gl::ExtensionSupported(
      reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS)),
      "GL_OES_EGL_image_external");
  return supported;
}

// True for YUV formats that must be sampled through GL_TEXTURE_EXTERNAL_OES so
// the driver applies the YUV->RGB conversion — planar (NV12, ...) and packed
// (YUYV/UYVY) alike. Packed RGB returns false and stays on GL_TEXTURE_2D.
bool NeedsExternalOes(uint32_t fourcc) {
  switch (fourcc) {
    case DRM_FORMAT_NV12:
    case DRM_FORMAT_NV21:
    case DRM_FORMAT_NV16:
    case DRM_FORMAT_NV61:
    case DRM_FORMAT_YUV420:
    case DRM_FORMAT_YVU420:
    case DRM_FORMAT_YUV422:
    case DRM_FORMAT_YUV444:
    case DRM_FORMAT_P010:
    case DRM_FORMAT_YUYV:
    case DRM_FORMAT_UYVY:
      return true;
    default:
      return false;
  }
}
}  // namespace

bool EglDmabufImporter::Init(void* egl_display) {
  if (egl_display == nullptr) {
    return false;
  }
  egl_display_ = egl_display;
  create_image_ =
      reinterpret_cast<void*>(eglGetProcAddress("eglCreateImageKHR"));
  destroy_image_ =
      reinterpret_cast<void*>(eglGetProcAddress("eglDestroyImageKHR"));
  image_target_texture_ = reinterpret_cast<void*>(
      eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (!ready()) {
    ihs::log::warn(
        "[EglDmabufImporter] EGL_EXT_image_dma_buf_import unavailable "
        "(eglCreateImageKHR/glEGLImageTargetTexture2DOES not resolved); "
        "dma-buf platform views fall back to the software floor");
    egl_display_ = nullptr;
    return false;
  }

  // Native fence sync is probed separately and is allowed to fail: a display
  // can import dma-bufs without being able to wait on a producer's sync_file,
  // and that combination still imports fine — it just paces implicitly.
  //
  // Check the extension string as well as the entry points. eglGetProcAddress
  // can return non-null for an extension the display does not actually support,
  // which is the same trap DrmCompositor::InitEglExtensions guards against for
  // IN_FENCE_FD, so the check here has the same three parts.
  const char* exts =
      eglQueryString(static_cast<EGLDisplay>(egl_display_), EGL_EXTENSIONS);
  const bool has_fence_sync =
      exts != nullptr && std::strstr(exts, "EGL_KHR_fence_sync") != nullptr;
  const bool has_native_fence =
      exts != nullptr &&
      std::strstr(exts, "EGL_ANDROID_native_fence_sync") != nullptr;
  const bool has_wait_sync =
      exts != nullptr && std::strstr(exts, "EGL_KHR_wait_sync") != nullptr;
  if (has_fence_sync && has_native_fence && has_wait_sync) {
    create_sync_ =
        reinterpret_cast<void*>(eglGetProcAddress("eglCreateSyncKHR"));
    destroy_sync_ =
        reinterpret_cast<void*>(eglGetProcAddress("eglDestroySyncKHR"));
    wait_sync_ = reinterpret_cast<void*>(eglGetProcAddress("eglWaitSyncKHR"));
  }
  // All three or none: a half-resolved set would let WaitAcquireFence create a
  // sync it cannot wait on or destroy, leaking the producer's fd every frame.
  if (!has_native_fence_sync()) {
    create_sync_ = nullptr;
    destroy_sync_ = nullptr;
    wait_sync_ = nullptr;
  }
  ihs::log::debug(
      "[EglDmabufImporter] explicit-sync acquire: {} (fence_sync={}, "
      "native_fence_sync={}, wait_sync={})",
      has_native_fence_sync() ? "available" : "unavailable",
      has_fence_sync ? "y" : "n", has_native_fence ? "y" : "n",
      has_wait_sync ? "y" : "n");
  return true;
}

bool EglDmabufImporter::Import(const IhsFrame& frame,
                               ImportedTexture* out) const {
  if (!ready() || out == nullptr) {
    return false;
  }
  const uint32_t planes = frame.plane_count == 0 ? 1 : frame.plane_count;
  if (planes > kPlaneAttribs.size()) {
    ihs::log::error("[EglDmabufImporter] plane_count {} > 4", planes);
    return false;
  }

  // Build the EGL_LINUX_DMA_BUF_EXT attribute list: geometry + fourcc +
  // per-plane fd/offset/pitch/modifier. attrib_list is EGLint*, so the 64-bit
  // modifier is split into its two 32-bit halves. Worst case: 6 ints for
  // geometry+fourcc (3 key/value pairs) + 10 per plane (fd, offset, pitch,
  // modifier-lo, modifier-hi — 5 key/value pairs) × 4 planes
  // + the EGL_NONE terminator.
  std::array<EGLint, 6 + 4 * 10 + 1> attribs{};
  size_t n = 0;
  attribs[n++] = EGL_WIDTH;
  attribs[n++] = static_cast<EGLint>(frame.width);
  attribs[n++] = EGL_HEIGHT;
  attribs[n++] = static_cast<EGLint>(frame.height);
  attribs[n++] = EGL_LINUX_DRM_FOURCC_EXT;
  attribs[n++] = static_cast<EGLint>(frame.format.fourcc);
  const uint64_t modifier = frame.format.modifier;
  for (uint32_t p = 0; p < planes; ++p) {
    const PlaneAttribs& a = kPlaneAttribs[p];
    attribs[n++] = a.fd;
    attribs[n++] = frame.plane_fd[p];
    attribs[n++] = a.offset;
    attribs[n++] = static_cast<EGLint>(frame.plane_offset[p]);
    attribs[n++] = a.pitch;
    attribs[n++] = static_cast<EGLint>(frame.plane_stride[p]);
    if (modifier != DRM_FORMAT_MOD_INVALID) {
      attribs[n++] = a.modifier_lo;
      attribs[n++] = static_cast<EGLint>(modifier & 0xffffffffULL);
      attribs[n++] = a.modifier_hi;
      attribs[n++] = static_cast<EGLint>(modifier >> 32);
    }
  }
  attribs[n++] = EGL_NONE;

  auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(create_image_);
  const EGLImageKHR image = create_image(  // NOLINT(misc-misplaced-const)
      static_cast<EGLDisplay>(egl_display_), EGL_NO_CONTEXT,
      EGL_LINUX_DMA_BUF_EXT, static_cast<EGLClientBuffer>(nullptr),
      attribs.data());
  // Contract: consume the plane fds only on success (EGL dup's them, so we
  // close ours below). On failure leave them for the caller to close.
  if (image == EGL_NO_IMAGE_KHR) {
    ihs::log::error(
        "[EglDmabufImporter] eglCreateImageKHR(LINUX_DMA_BUF, fourcc=0x{:x}, "
        "mod=0x{:x}, {}x{}, planes={}): 0x{:x}",
        frame.format.fourcc, modifier, frame.width, frame.height, planes,
        eglGetError());
    return false;
  }
  const bool external = NeedsExternalOes(frame.format.fourcc);
  if (external && !HasExternalImage()) {
    // Binding to a target the context does not implement raises
    // GL_INVALID_ENUM and would otherwise return a half-built texture. Undo
    // the image and report failure, which leaves the plane fds to the caller
    // as the contract says.
    auto destroy = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(destroy_image_);
    if (destroy != nullptr) {
      destroy(static_cast<EGLDisplay>(egl_display_), image);
    }
    ihs::log::error(
        "[EglDmabufImporter] planar fourcc=0x{:x} needs "
        "GL_OES_EGL_image_external, which this context lacks",
        frame.format.fourcc);
    return false;
  }

  for (uint32_t p = 0; p < planes; ++p) {
    close(frame.plane_fd[p]);  // EGL dup'd them
  }

  // A YUV image carries its colour conversion in the sampler, which only
  // GL_TEXTURE_EXTERNAL_OES provides. Bound as GL_TEXTURE_2D the driver
  // exposes the first plane alone, so a plain sampler2D reads luma into red.
  const GLenum target = external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(target, tex);
  auto image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
      image_target_texture_);
  image_target(target, static_cast<GLeglImageOES>(image));
  glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  // External textures support only CLAMP_TO_EDGE and no mipmaps; the same
  // settings suit the 2D path, so there is one set for both.
  glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(target, 0);

  out->texture = tex;
  out->egl_image = image;
  out->width = frame.width;
  out->height = frame.height;
  out->external = external;
  return true;
}

void EglDmabufImporter::Destroy(ImportedTexture* out) const {
  if (out == nullptr) {
    return;
  }
  if (out->texture != 0) {
    const GLuint tex = out->texture;
    glDeleteTextures(1, &tex);
    out->texture = 0;
  }
  if (out->egl_image != nullptr && destroy_image_ != nullptr) {
    auto destroy_image =
        reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(destroy_image_);
    destroy_image(static_cast<EGLDisplay>(egl_display_),
                  static_cast<EGLImageKHR>(out->egl_image));
    out->egl_image = nullptr;
  }
}

bool EglDmabufImporter::WaitAcquireFence(int fence_fd) const {
  if (!has_native_fence_sync() || fence_fd < 0) {
    return false;
  }
  auto create_sync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(create_sync_);
  auto destroy_sync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(destroy_sync_);
  auto wait_sync = reinterpret_cast<PFNEGLWAITSYNCKHRPROC>(wait_sync_);
  const auto dpy = static_cast<EGLDisplay>(egl_display_);

  // eglCreateSyncKHR takes ownership of the fd on success and closes it when
  // the sync is destroyed; on failure the fd is left to us. Reporting that
  // split back through the return value is this function's whole contract --
  // get it backwards and every frame either double-closes or leaks an fd.
  const EGLint attribs[] = {EGL_SYNC_NATIVE_FENCE_FD_ANDROID, fence_fd,
                            EGL_NONE};
  const EGLSyncKHR sync =  // NOLINT(misc-misplaced-const)
      create_sync(dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs);
  if (sync == EGL_NO_SYNC_KHR) {
    ihs::log::warn(
        "[EglDmabufImporter] eglCreateSyncKHR(NATIVE_FENCE, fd={}): 0x{:x}; "
        "falling back to a CPU wait",
        fence_fd, eglGetError());
    return false;  // fd untouched — still the caller's to close
  }

  // Server-side wait: queues the wait into the GL command stream so the GPU
  // blocks before sampling, and this thread does not block at all. Destroying
  // the sync afterwards does not cancel a wait already queued against it.
  const EGLint rc = wait_sync(dpy, sync, 0);
  destroy_sync(dpy, sync);
  if (rc != EGL_TRUE) {
    // The fd is gone either way (EGL took it with the sync), so there is
    // nothing to fall back to. Sample best-effort and say so, matching the CPU
    // path's policy on a timed-out fence.
    ihs::log::warn(
        "[EglDmabufImporter] eglWaitSyncKHR: 0x{:x}; sampling anyway — frame "
        "may tear",
        eglGetError());
  }
  return true;
}
