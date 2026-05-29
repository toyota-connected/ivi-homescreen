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

#include "backend/wayland_egl/egl_backing_store.h"
#include "logging/logging.h"

#include <GLES2/gl2ext.h>

#include "backend/wayland_egl/gl_caps.h"
#include "config/common.h"
#include "logging.h"

namespace {

#if BUILD_EGL_ENABLE_MULTISAMPLE
constexpr GLsizei kMsaaSamples = 4;
#endif

// Sized color internal formats are ES3 core and ES2 via OES_rgb8_rgba8.
// When unavailable we fall back to unsized GL_RGBA / GL_RGB, which is
// universally supported but may be implementation-defined width.
struct ColorFormat {
  GLenum internal_format;
  GLenum format;
};

ColorFormat PickColorFormat(const GlCaps* caps) {
#if BUILD_EGL_TRANSPARENCY
  if (caps && caps->has_rgb8_rgba8) {
    return {GL_RGBA8_OES, GL_RGBA};
  }
  return {GL_RGBA, GL_RGBA};
#else
  if (caps && caps->has_rgb8_rgba8) {
    return {GL_RGB8_OES, GL_RGB};
  }
  return {GL_RGB, GL_RGB};
#endif
}

}  // namespace

EglFboBackingStore::EglFboBackingStore(int32_t width,
                                       int32_t height,
                                       const GlCaps* caps)
    : width_(width), height_(height) {
  const ColorFormat color = PickColorFormat(caps);
  const bool msaa_ok = caps && caps->has_multisampled_renderbuffer &&
                       caps->renderbuffer_storage_multisample != nullptr;

  glGenFramebuffers(1, &framebuffer_);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);

  glGenTextures(1, &color_texture_);
  glBindTexture(GL_TEXTURE_2D, color_texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(color.internal_format),
               width_, height_, 0, color.format, GL_UNSIGNED_BYTE, nullptr);

#if BUILD_EGL_ENABLE_MULTISAMPLE
  if (msaa_ok) {
    // Multisample renderbuffer used as the color attachment; the texture is
    // the resolve target and gets blitted/composited during presentation.
    glGenRenderbuffers(1, &color_rb_msaa_);
    glBindRenderbuffer(GL_RENDERBUFFER, color_rb_msaa_);
    caps->renderbuffer_storage_multisample(
        GL_RENDERBUFFER, kMsaaSamples, color.internal_format, width_, height_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, color_rb_msaa_);
  } else {
    spdlog::warn(
        "EglFboBackingStore: BUILD_EGL_ENABLE_MULTISAMPLE set but no "
        "multisample renderbuffer support; dropping MSAA.");
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           color_texture_, 0);
  }
#else
  (void)msaa_ok;
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         color_texture_, 0);
#endif

#if BUILD_EGL_ENABLE_3D
  glGenRenderbuffers(1, &depth_stencil_rb_);
  glBindRenderbuffer(GL_RENDERBUFFER, depth_stencil_rb_);
  const bool packed_ds = caps && caps->has_packed_depth_stencil;
  const GLenum ds_format =
      packed_ds ? GL_DEPTH24_STENCIL8_OES : GL_DEPTH_COMPONENT16;
#if BUILD_EGL_ENABLE_MULTISAMPLE
  if (msaa_ok) {
    caps->renderbuffer_storage_multisample(GL_RENDERBUFFER, kMsaaSamples,
                                           ds_format, width_, height_);
  } else {
    glRenderbufferStorage(GL_RENDERBUFFER, ds_format, width_, height_);
  }
#else
  glRenderbufferStorage(GL_RENDERBUFFER, ds_format, width_, height_);
#endif
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, depth_stencil_rb_);
  if (packed_ds) {
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, depth_stencil_rb_);
  }
#endif

  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    spdlog::error(
        "EglFboBackingStore: framebuffer incomplete (0x{:x}) for {}x{}", status,
        width_, height_);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
}

EglFboBackingStore::~EglFboBackingStore() {
  if (framebuffer_) {
    glDeleteFramebuffers(1, &framebuffer_);
  }
  if (color_texture_) {
    glDeleteTextures(1, &color_texture_);
  }
  if (depth_stencil_rb_) {
    glDeleteRenderbuffers(1, &depth_stencil_rb_);
  }
  if (color_rb_msaa_) {
    glDeleteRenderbuffers(1, &color_rb_msaa_);
  }
}

EglTextureBackingStore::EglTextureBackingStore(int32_t width,
                                               int32_t height,
                                               const GlCaps* caps)
    : width_(width), height_(height) {
  const ColorFormat color = PickColorFormat(caps);
  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(color.internal_format),
               width_, height_, 0, color.format, GL_UNSIGNED_BYTE, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
}

EglTextureBackingStore::~EglTextureBackingStore() {
  if (texture_) {
    glDeleteTextures(1, &texture_);
  }
}
