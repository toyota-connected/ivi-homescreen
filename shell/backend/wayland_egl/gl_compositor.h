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

#include <cstddef>
#include <functional>

#include "view/compositor_surface_interface.h"
#include "view/layer_geometry.h"

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

  /**
   * @brief Draw a platform-view layer: @p tex sampled through @p uv into the
   *        destination rect of @p dst_fbo.
   *
   * @p uv maps the destination rect to texture coordinates (see UvAffine: its
   * t axis runs top to bottom, and v = 0 is the texture's first row), which is
   * how a source crop and a buffer transform reach the draw. Always the quad
   * path: a blit can neither crop through a transform nor sample an external
   * image.
   *
   * @param dst_x,dst_y  Destination origin, OpenGL coordinates (bottom-left).
   * @param opaque       Draw alpha as 1, for an XRGB-style buffer whose alpha
   *                     channel is undefined, and a layer known to cover its
   *                     rect. Blending still follows @p blend.
   */
  void CompositeLayerToFbo(GLuint dst_fbo,
                           GLuint tex,
                           bool external,
                           const UvAffine& uv,
                           GLint dst_x,
                           GLint dst_y,
                           GLsizei dst_w,
                           GLsizei dst_h,
                           bool blend,
                           bool opaque);

  /**
   * @brief Draw every layer of @p surface, bottom to top, into its view rect.
   *
   * @param view        The view's rect on the target, in pixels with a
   *                    top-left origin (the FlutterLayer's offset and size).
   * @param fb_height   The target's height. Unused for a top-first target.
   * @param target_top_first  The target is scanned out first row first, so
   *                    GL's y = 0 is its top (an FBO fed to a KMS plane).
   *                    False for a window's default framebuffer, where GL's
   *                    bottom-left origin is the bottom.
   * @param blend_first Whether the first layer drawn blends (something is
   *                    already under it). Later layers blend unless opaque.
   * @param on_drawn    Called for each layer drawn, e.g. to queue its release.
   * @return            The number of layers drawn.
   *
   * Imports each layer's latest frame (GetLayerGlTexture), so it runs on the
   * raster thread with the context current, like any texture composite.
   */
  size_t CompositeSurfaceLayers(
      GLuint dst_fbo,
      const ICompositorSurface& surface,
      const RectI& view,
      GLint fb_height,
      bool target_top_first,
      bool blend_first,
      const std::function<void(const ICompositorSurface::GlLayerTexture&)>&
          on_drawn = nullptr);

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
  // Links the samplerExternalOES variant of the quad program, used for YUV
  // platform-view frames (planar or packed). Leaves program_external_ at 0 when
  // the driver lacks GL_OES_EGL_image_external.
  void BuildExternalProgram();
  void CompositeViaQuadExternal(GLuint tex,
                                GLint dst_x,
                                GLint dst_y,
                                GLsizei dst_w,
                                GLsizei dst_h,
                                bool blend,
                                const UvAffine& uv,
                                bool opaque);
  // @external selects the samplerExternalOES program and the
  // GL_TEXTURE_EXTERNAL_OES bind target, which planar YUV requires.
  void CompositeViaQuad(GLuint src_color_tex,
                        GLint dst_x,
                        GLint dst_y,
                        GLsizei dst_w,
                        GLsizei dst_h,
                        bool blend,
                        const UvAffine& uv,
                        bool opaque,
                        bool external);
  // The texture coordinates the legacy flip_y flag stood for: a top-first
  // texture needs none, a bottom-first one (a GL render target) a V flip.
  static UvAffine UvForFlip(bool flip_y);
  // Upload @p uv and @p opaque to a program's uniforms.
  static void SetLayerUniforms(GLint uni_uv_u,
                               GLint uni_uv_v,
                               GLint uni_opaque,
                               const UvAffine& uv,
                               bool opaque);
  void EmitPersistentQuadState();
  void TearDownPersistentQuadState();

  // u = dot(u_uv_u, (s, t, 1)), v = dot(u_uv_v, (s, t, 1)); see UvAffine.
  GLint uni_uv_u_{-1};
  GLint uni_uv_v_{-1};
  GLint uni_opaque_{-1};

  // The external-sampler variant. A planar YUV dma-buf is bound to
  // GL_TEXTURE_EXTERNAL_OES and can only be read through this program;
  // sampling it with the sampler2D one yields the luma plane in red.
  GLuint program_external_{0};
  GLint attr_pos_external_{-1};
  GLint attr_uv_external_{-1};
  GLint uni_tex_external_{-1};
  GLint uni_uv_u_external_{-1};
  GLint uni_uv_v_external_{-1};
  GLint uni_opaque_external_{-1};
};
