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

#include <GLES2/gl2.h>

struct GlCaps;

/**
 * @brief Composites a backing store onto the default framebuffer.
 *
 * Two paths, chosen per @c GlCaps:
 *   - ES3 / NV_framebuffer_blit / ANGLE_framebuffer_blit: uses
 *     @c glBlitFramebuffer.
 *   - Pure ES2: draws a textured triangle strip using a cached shader
 *     program and VBO. Linear filtering is used; no blending.
 *
 * Lazily initializes its shader resources the first time the quad path
 * is needed; releases them in the destructor. Must be destroyed with the
 * EGL context still current.
 */
class GlCompositor {
 public:
  explicit GlCompositor(const GlCaps* caps);
  ~GlCompositor();

  GlCompositor(const GlCompositor&) = delete;
  GlCompositor& operator=(const GlCompositor&) = delete;

  /**
   * @brief Composite the given backing store onto the default framebuffer.
   *
   * @param src_fbo         Source framebuffer containing @p src_color_texture.
   *                        Used by the blit path.
   * @param src_color_tex   Source color texture. Used by the quad path.
   * @param src_w,src_h     Source dimensions in pixels.
   * @param dst_x,dst_y     Destination origin on the default framebuffer,
   *                        in OpenGL coordinates (origin bottom-left).
   * @param dst_w,dst_h     Destination dimensions in pixels.
   * @param blend           When true, force the quad path with premultiplied
   *                        alpha blending so transparent pixels preserve the
   *                        underlying framebuffer content. Required for
   *                        overlay layers stacked on top of other layers.
   * @param flip_y          When true, flip the source vertically. Use for
   *                        textures drawn in GL-native (bottom-left) origin
   *                        that need to land in Flutter's top-down layout.
   */
  void CompositeToDefault(GLuint src_fbo,
                          GLuint src_color_tex,
                          GLsizei src_w,
                          GLsizei src_h,
                          GLint dst_x,
                          GLint dst_y,
                          GLsizei dst_w,
                          GLsizei dst_h,
                          bool blend = false,
                          bool flip_y = false);

 private:
  const GlCaps* caps_;

  // Fallback quad resources (lazy-initialized on first use).
  bool quad_initialized_{false};
  GLuint program_{0};
  GLuint vbo_{0};
  GLint attr_pos_{-1};
  GLint attr_uv_{-1};
  GLint uni_tex_{-1};

  bool EnsureQuad();
  void CompositeViaQuad(GLuint src_color_tex,
                        GLint dst_x,
                        GLint dst_y,
                        GLsizei dst_w,
                        GLsizei dst_h,
                        bool blend,
                        bool flip_y);

  GLint uni_uv_y_scale_{-1};
  GLint uni_uv_y_offset_{-1};
};
