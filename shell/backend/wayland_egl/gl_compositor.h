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
   * @brief Composite the given backing store into @p dst_fbo.
   *
   * @param dst_fbo         Destination framebuffer; 0 selects the default
   *                        framebuffer.
   * @param src_fbo         Source framebuffer containing @p src_color_tex.
   *                        Used by the blit path. Pass 0 to force the quad
   *                        path (e.g. raw platform-view textures with no
   *                        associated FBO).
   * @param src_color_tex   Source color texture. Used by the quad path.
   * @param src_w,src_h     Source dimensions in pixels.
   * @param dst_x,dst_y     Destination origin in @p dst_fbo, in OpenGL
   *                        coordinates (origin bottom-left).
   * @param dst_w,dst_h     Destination dimensions in pixels.
   * @param blend           When true, force the quad path with premultiplied
   *                        alpha blending so transparent pixels preserve the
   *                        underlying framebuffer content. Required for
   *                        overlay layers stacked on top of other layers.
   * @param flip_y          When true, flip the source vertically. Use for
   *                        textures drawn in GL-native (bottom-left) origin
   *                        that need to land in Flutter's top-down layout.
   */
  void CompositeToFbo(GLuint dst_fbo,
                      GLuint src_fbo,
                      GLuint src_color_tex,
                      GLsizei src_w,
                      GLsizei src_h,
                      GLint dst_x,
                      GLint dst_y,
                      GLsizei dst_w,
                      GLsizei dst_h,
                      bool blend = false,
                      bool flip_y = false,
                      bool external = false);

  // Thin wrapper: composite into the default framebuffer (FBO 0).
  void CompositeToDefault(GLuint src_fbo,
                          GLuint src_color_tex,
                          GLsizei src_w,
                          GLsizei src_h,
                          GLint dst_x,
                          GLint dst_y,
                          GLsizei dst_w,
                          GLsizei dst_h,
                          bool blend = false,
                          bool flip_y = false,
                          bool external = false) {
    CompositeToFbo(0, src_fbo, src_color_tex, src_w, src_h, dst_x, dst_y, dst_w,
                   dst_h, blend, flip_y, external);
  }

  /**
   * @brief Mark the start/end of a sequence of composite calls.
   *
   * Between BeginFrame and EndFrame the persistent GL quad state
   * (program, VBO, vertex-attrib pointers, depth/stencil/scissor/cull
   * disables, sampler uniform) is emitted once for the first quad-path
   * composite and torn down on EndFrame, so subsequent quad composites
   * within the frame skip ~13 redundant GL calls each. If BeginFrame is
   * not called, CompositeToDefault keeps its per-call setup+teardown
   * behaviour for one-off compositions.
   *
   * Calls must be balanced. Re-entrancy is not supported.
   */
  void BeginFrame();
  void EndFrame();

 private:
  const GlCaps* caps_;

  // Fallback quad resources (lazy-initialized on first use).
  bool quad_initialized_{false};
  GLuint program_{0};
  GLuint vbo_{0};
  GLint attr_pos_{-1};
  GLint attr_uv_{-1};
  GLint uni_tex_{-1};

  // Per-frame batching state. frame_open_ is toggled by BeginFrame/EndFrame;
  // persistent_state_emitted_ tracks whether the once-per-frame setup has
  // actually been issued (deferred until the first quad-path composite, so
  // a frame composed entirely of blit-path layers pays nothing).
  bool frame_open_{false};
  bool persistent_state_emitted_{false};
  bool blend_enabled_{false};
  GLuint bound_tex_{0};

  bool EnsureQuad();
  // Links the samplerExternalOES variant of the quad program, used for planar
  // YUV platform-view frames. Leaves program_external_ at 0 when the driver
  // lacks GL_OES_EGL_image_external.
  void BuildExternalProgram();
  void CompositeViaQuadExternal(GLuint tex,
                                GLint dst_x,
                                GLint dst_y,
                                GLsizei dst_w,
                                GLsizei dst_h,
                                bool blend,
                                bool flip_y);
  // @external selects the samplerExternalOES program and the
  // GL_TEXTURE_EXTERNAL_OES bind target, which planar YUV requires.
  void CompositeViaQuad(GLuint src_color_tex,
                        GLint dst_x,
                        GLint dst_y,
                        GLsizei dst_w,
                        GLsizei dst_h,
                        bool blend,
                        bool flip_y,
                        bool external);
  void EmitPersistentQuadState();
  void TearDownPersistentQuadState();

  GLint uni_uv_y_scale_{-1};
  GLint uni_uv_y_offset_{-1};

  // The external-sampler variant. A planar YUV dma-buf is bound to
  // GL_TEXTURE_EXTERNAL_OES and can only be read through this program;
  // sampling it with the sampler2D one yields the luma plane in red.
  GLuint program_external_{0};
  GLint attr_pos_external_{-1};
  GLint attr_uv_external_{-1};
  GLint uni_tex_external_{-1};
  GLint uni_uv_y_scale_external_{-1};
  GLint uni_uv_y_offset_external_{-1};
};
