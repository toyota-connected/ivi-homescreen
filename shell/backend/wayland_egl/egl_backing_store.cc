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

// GLES3 is required when BUILD_COMPOSITOR is enabled — the backing store
// relies on glBlitFramebuffer, glRenderbufferStorageMultisample, and
// sized internal formats (GL_RGBA8, GL_DEPTH24_STENCIL8) that are core
// in GLES3 but only extensions in GLES2.
#include <GLES3/gl3.h>

#include "config/common.h"
#include "logging.h"

namespace {

// Format the color attachment. With BUILD_EGL_TRANSPARENCY the FBO keeps an
// alpha channel; otherwise RGB8 is sufficient.
#if BUILD_EGL_TRANSPARENCY
constexpr GLenum kColorInternalFormat = GL_RGBA8;
constexpr GLenum kColorFormat = GL_RGBA;
#else
constexpr GLenum kColorInternalFormat = GL_RGB8;
constexpr GLenum kColorFormat = GL_RGB;
#endif

#if BUILD_EGL_ENABLE_MULTISAMPLE
constexpr GLsizei kMsaaSamples = 4;
#endif

}  // namespace

EglFboBackingStore::EglFboBackingStore(int32_t width, int32_t height)
    : width_(width), height_(height) {
  glGenFramebuffers(1, &framebuffer_);
  glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);

  glGenTextures(1, &color_texture_);
  glBindTexture(GL_TEXTURE_2D, color_texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, kColorInternalFormat, width_, height_, 0,
               kColorFormat, GL_UNSIGNED_BYTE, nullptr);

#if BUILD_EGL_ENABLE_MULTISAMPLE
  // Multisample renderbuffer used as the color attachment; the texture is the
  // resolve target and gets blitted to during presentation.
  glGenRenderbuffers(1, &color_rb_msaa_);
  glBindRenderbuffer(GL_RENDERBUFFER, color_rb_msaa_);
  glRenderbufferStorageMultisample(GL_RENDERBUFFER, kMsaaSamples,
                                   kColorInternalFormat, width_, height_);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                            GL_RENDERBUFFER, color_rb_msaa_);
#else
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         color_texture_, 0);
#endif

#if BUILD_EGL_ENABLE_3D
  glGenRenderbuffers(1, &depth_stencil_rb_);
  glBindRenderbuffer(GL_RENDERBUFFER, depth_stencil_rb_);
#if BUILD_EGL_ENABLE_MULTISAMPLE
  glRenderbufferStorageMultisample(GL_RENDERBUFFER, kMsaaSamples,
                                   GL_DEPTH24_STENCIL8, width_, height_);
#else
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width_, height_);
#endif
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, depth_stencil_rb_);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                            GL_RENDERBUFFER, depth_stencil_rb_);
#endif

  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    spdlog::error(
        "EglFboBackingStore: framebuffer incomplete (0x{:x}) for {}x{}",
        status, width_, height_);
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

EglTextureBackingStore::EglTextureBackingStore(int32_t width, int32_t height)
    : width_(width), height_(height) {
  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, kColorInternalFormat, width_, height_, 0,
               kColorFormat, GL_UNSIGNED_BYTE, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);
}

EglTextureBackingStore::~EglTextureBackingStore() {
  if (texture_) {
    glDeleteTextures(1, &texture_);
  }
}
