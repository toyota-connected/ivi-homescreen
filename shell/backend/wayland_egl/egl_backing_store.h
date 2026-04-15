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

/**
 * @brief FBO-backed EGL backing store.
 *
 * The engine renders into @c framebuffer and the compositor blits the
 * attached @c color_texture to the window or a subsurface during
 * @c PresentLayers. Constructed on the rasterizer thread with the EGL
 * context current; destructor deletes GL objects (also on rasterizer
 * thread — call @c BackingStorePool::Flush while the context is still
 * current).
 *
 * When @c BUILD_EGL_ENABLE_3D is set the FBO gets a depth/stencil
 * renderbuffer; when @c BUILD_EGL_ENABLE_MULTISAMPLE is set the color
 * attachment is allocated as a multisample renderbuffer plus a resolve
 * texture.
 */
class EglFboBackingStore {
 public:
  EglFboBackingStore(int32_t width, int32_t height);
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
 * Layout matches @c EglFboBackingStore but the engine is pointed at the
 * color texture instead of the framebuffer. Useful for overlay layers that
 * plugins want to sample (e.g., blur pass).
 */
class EglTextureBackingStore {
 public:
  EglTextureBackingStore(int32_t width, int32_t height);
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
