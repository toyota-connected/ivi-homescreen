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

#include <GLES2/gl2.h>

struct GlCaps;

/**
 * @brief FBO-backed EGL backing store.
 *
 * The engine renders into @c framebuffer and the compositor blits (or
 * quad-shader-composites) the attached @c color_texture to the window or a
 * subsurface during @c PresentLayers. Constructed on the rasterizer thread
 * with the EGL context current; destructor deletes GL objects (also on the
 * rasterizer thread — call @c BackingStorePool::Flush while the context is
 * still current).
 *
 * Format choices adapt to the runtime GL version via @c GlCaps:
 *   - Color attachment uses sized formats (GL_RGBA8 / GL_RGB8 / _OES) when
 *     available, else falls back to unsized GL_RGBA / GL_RGB with
 *     GL_UNSIGNED_BYTE, which is ES2 core.
 *   - Depth/stencil uses packed GL_DEPTH24_STENCIL8_OES when the extension
 *     or ES3 is present; otherwise a plain GL_DEPTH_COMPONENT16 renderbuffer
 *     is used and stencil is dropped.
 *   - MSAA is skipped entirely when no multisample-renderbuffer path is
 *     advertised.
 *
 * The caps pointer must outlive this object (the backend owns the caps).
 */
class EglFboBackingStore {
 public:
  EglFboBackingStore(int32_t width, int32_t height, const GlCaps* caps);
  ~EglFboBackingStore();

  EglFboBackingStore(const EglFboBackingStore&) = delete;
  EglFboBackingStore& operator=(const EglFboBackingStore&) = delete;

  [[nodiscard]] int32_t Width() const { return width_; }
  [[nodiscard]] int32_t Height() const { return height_; }
  [[nodiscard]] GLuint Framebuffer() const { return framebuffer_; }
  [[nodiscard]] GLuint ColorTexture() const { return color_texture_; }

 private:
  int32_t width_{0};
  int32_t height_{0};
  GLuint framebuffer_{0};
  GLuint color_texture_{0};
  GLuint depth_stencil_rb_{0};
  GLuint color_rb_msaa_{0};  // used only when MSAA is enabled
};

/**
 * @brief Texture-backed EGL backing store for sampleable output.
 *
 * Plain GL_TEXTURE_2D with RGBA storage. Used for overlay layers that
 * plugins want to sample.
 */
class EglTextureBackingStore {
 public:
  EglTextureBackingStore(int32_t width, int32_t height, const GlCaps* caps);
  ~EglTextureBackingStore();

  EglTextureBackingStore(const EglTextureBackingStore&) = delete;
  EglTextureBackingStore& operator=(const EglTextureBackingStore&) = delete;

  [[nodiscard]] int32_t Width() const { return width_; }
  [[nodiscard]] int32_t Height() const { return height_; }
  [[nodiscard]] GLuint Texture() const { return texture_; }

 private:
  int32_t width_{0};
  int32_t height_{0};
  GLuint texture_{0};
};
