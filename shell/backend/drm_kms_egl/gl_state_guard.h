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

#include <array>
#include <cstddef>

#include <GLES2/gl2.h>

namespace homescreen {

// Saves the GL state a draw over the engine's frame touches, and puts it back
// on destruction.
//
// The engine's renderer (Skia) caches GL state and does NOT re-query it
// between frames. Any state a draw changes and leaves changed desyncs that
// cache and corrupts the next frame (e.g. a disabled scissor gives
// full-field flashing). So a draw on the raster thread between the engine's
// frames snapshots every piece of state it touches and restores it exactly,
// leaving the context byte-for-byte as the engine left it.
//
// @p attribs are the vertex attribute locations the draw uses; their array
// state is saved and restored with the rest.
class ScopedGlState {
 public:
  static constexpr size_t kMaxAttribs = 2;

  explicit ScopedGlState(const std::array<GLint, kMaxAttribs>& attribs)
      : locs_(attribs) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog_);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo_);
    glGetIntegerv(GL_VIEWPORT, vp_.data());
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buf_);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_tex_);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex2d_);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color_.data());
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask_.data());

    blend_ = glIsEnabled(GL_BLEND);
    scissor_ = glIsEnabled(GL_SCISSOR_TEST);
    depth_ = glIsEnabled(GL_DEPTH_TEST);
    cull_ = glIsEnabled(GL_CULL_FACE);
    stencil_ = glIsEnabled(GL_STENCIL_TEST);
    glGetIntegerv(GL_BLEND_SRC_RGB, &bsrc_rgb_);
    glGetIntegerv(GL_BLEND_DST_RGB, &bdst_rgb_);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &bsrc_a_);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &bdst_a_);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &beq_rgb_);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &beq_a_);

    for (size_t i = 0; i < locs_.size(); ++i) {
      if (locs_[i] < 0) {
        continue;
      }
      const auto loc = static_cast<GLuint>(locs_[i]);
      AttribState& s = attribs_[i];
      glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &s.enabled);
      glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_SIZE, &s.size);
      glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_TYPE, &s.type);
      glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                          &s.normalized);
      glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &s.stride);
      glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                          &s.buffer);
      glGetVertexAttribPointerv(loc, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                                &s.pointer);
    }
  }

  ~ScopedGlState() {
    for (size_t i = 0; i < locs_.size(); ++i) {
      if (locs_[i] < 0) {
        continue;
      }
      const auto loc = static_cast<GLuint>(locs_[i]);
      const AttribState& s = attribs_[i];
      glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(s.buffer));
      glVertexAttribPointer(loc, s.size, static_cast<GLenum>(s.type),
                            static_cast<GLboolean>(s.normalized), s.stride,
                            s.pointer);
      if (s.enabled != 0) {
        glEnableVertexAttribArray(loc);
      } else {
        glDisableVertexAttribArray(loc);
      }
    }
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buf_));

    glBlendEquationSeparate(static_cast<GLenum>(beq_rgb_),
                            static_cast<GLenum>(beq_a_));
    glBlendFuncSeparate(
        static_cast<GLenum>(bsrc_rgb_), static_cast<GLenum>(bdst_rgb_),
        static_cast<GLenum>(bsrc_a_), static_cast<GLenum>(bdst_a_));
    SetEnabled(GL_BLEND, blend_);
    SetEnabled(GL_SCISSOR_TEST, scissor_);
    SetEnabled(GL_DEPTH_TEST, depth_);
    SetEnabled(GL_CULL_FACE, cull_);
    SetEnabled(GL_STENCIL_TEST, stencil_);
    glClearColor(clear_color_[0], clear_color_[1], clear_color_[2],
                 clear_color_[3]);
    glColorMask(color_mask_[0], color_mask_[1], color_mask_[2], color_mask_[3]);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex2d_));
    glActiveTexture(static_cast<GLenum>(active_tex_));
    glUseProgram(static_cast<GLuint>(prog_));
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fbo_));
    glViewport(vp_[0], vp_[1], vp_[2], vp_[3]);
  }

  ScopedGlState(const ScopedGlState&) = delete;
  ScopedGlState& operator=(const ScopedGlState&) = delete;

 private:
  struct AttribState {
    GLint enabled{0};
    GLint size{4};
    GLint type{GL_FLOAT};
    GLint normalized{GL_FALSE};
    GLint stride{0};
    GLint buffer{0};
    void* pointer{nullptr};
  };

  static void SetEnabled(const GLenum cap, const GLboolean on) {
    if (on != GL_FALSE) {
      glEnable(cap);
    } else {
      glDisable(cap);
    }
  }

  std::array<GLint, kMaxAttribs> locs_;
  std::array<AttribState, kMaxAttribs> attribs_{};
  GLint prog_{0};
  GLint fbo_{0};
  std::array<GLint, 4> vp_{};
  GLint array_buf_{0};
  GLint active_tex_{GL_TEXTURE0};
  GLint tex2d_{0};
  std::array<GLfloat, 4> clear_color_{};
  std::array<GLboolean, 4> color_mask_{GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
  GLboolean blend_{GL_FALSE};
  GLboolean scissor_{GL_FALSE};
  GLboolean depth_{GL_FALSE};
  GLboolean cull_{GL_FALSE};
  GLboolean stencil_{GL_FALSE};
  GLint bsrc_rgb_{GL_ONE};
  GLint bdst_rgb_{GL_ZERO};
  GLint bsrc_a_{GL_ONE};
  GLint bdst_a_{GL_ZERO};
  GLint beq_rgb_{GL_FUNC_ADD};
  GLint beq_a_{GL_FUNC_ADD};
};

}  // namespace homescreen
