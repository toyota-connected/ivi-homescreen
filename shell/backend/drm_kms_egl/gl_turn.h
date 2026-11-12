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

#include "view/display_rotation.h"

namespace homescreen {

// --drm-rotation for drm-kms-egl in a build without BUILD_COMPOSITOR, where
// the engine cannot turn the picture itself (#848).
//
// The engine renders upright into Fbo(), an offscreen framebuffer at the
// render extent, and Draw() turns it onto the window framebuffer (FBO 0, the
// panel) at present. That is one full-panel draw per frame; the engine's own
// surface_transformation avoids it, but only Skia honors that, and only for a
// picture that fills the panel (see DrmBackend::GetRenderConfig).
//
// Rasterizer thread only, with the EGL context current.
class GlTurn final {
 public:
  GlTurn() = default;
  ~GlTurn() = default;

  GlTurn(const GlTurn&) = delete;
  GlTurn& operator=(const GlTurn&) = delete;

  // The framebuffer the engine renders into, holding @p w x @p h. Built on
  // first use, and its attachments reallocated when the extent changes; the
  // id stays the same. 0 when it cannot be built, which renders upright into
  // the panel rather than not at all (logged once).
  GLuint Fbo(uint32_t w, uint32_t h);

  // Draws the frame Fbo() holds onto FBO 0, turned by @p rotation and
  // centered, with black around it. No-op before Fbo() succeeded.
  void Draw(const DisplayRotation& rotation);

  // Releases the GL objects.
  void Destroy();

 private:
  bool EnsureProgram();
  bool Allocate(uint32_t w, uint32_t h);

  GLuint fbo_{0};
  GLuint color_{0};
  GLuint depth_stencil_{0};
  uint32_t w_{0};
  uint32_t h_{0};
  bool ready_{false};

  GLuint program_{0};
  GLint a_pos_{-1};
  GLint a_uv_{-1};
  GLint u_tex_{-1};
  bool failed_{false};
};

}  // namespace homescreen
