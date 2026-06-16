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

#include "backend/drm_kms_egl/gl_cursor.h"

#include <array>
#include <cstring>

#include "logging.h"

namespace homescreen {

namespace {

// Classic left-pointer arrow, 12x19 (same sprite as the software backend's
// SoftwareCursor). 'o' = black outline, 'X' = white fill, space = transparent.
// Hotspot is the tip at (0, 0). Each row is exactly kCursorW characters.
constexpr uint32_t kCursorW = 12;
constexpr uint32_t kCursorH = 19;
constexpr std::array<const char*, kCursorH> kArrow = {
    "o           ", "oo          ", "oXo         ", "oXXo        ",
    "oXXXo       ", "oXXXXo      ", "oXXXXXo     ", "oXXXXXXo    ",
    "oXXXXXXXo   ", "oXXXXXXXXo  ", "oXXXXXoooooo", "oXXoXXo     ",
    "oXo oXXo    ", "oo  oXXo    ", "o    oXXo   ", "     oXXo   ",
    "      oXXo  ", "      oXXo  ", "       oo   ",
};

constexpr char kVertexShader[] =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

constexpr char kFragmentShader[] =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

GLuint CompileShader(const GLenum type, const char* src) {
  const GLuint sh = glCreateShader(type);
  glShaderSource(sh, 1, &src, nullptr);
  glCompileShader(sh);
  GLint ok = GL_FALSE;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    char log[512] = {};
    glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
    spdlog::error("[GlCursor] shader compile failed: {}", log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

}  // namespace

GlCursor::GlCursor() : w_(kCursorW), h_(kCursorH) {
  // Build the premultiplied RGBA8888 sprite (memory order [R,G,B,A]). The
  // fill/outline colors are already opaque, so premultiplication is a no-op
  // on them; transparent texels are all-zero.
  rgba_.assign(static_cast<size_t>(w_) * h_ * 4, 0);
  for (uint32_t row = 0; row < h_; ++row) {
    const char* line = kArrow[row];
    const size_t len = std::strlen(line);
    for (uint32_t col = 0; col < w_; ++col) {
      const char c = col < len ? line[col] : ' ';
      uint8_t r = 0;
      uint8_t g = 0;
      uint8_t b = 0;
      uint8_t a = 0;
      if (c == 'X') {  // opaque white fill
        r = g = b = a = 0xFF;
      } else if (c == 'o') {  // opaque black outline
        a = 0xFF;
      }
      uint8_t* px = rgba_.data() + (static_cast<size_t>(row) * w_ + col) * 4;
      px[0] = r;
      px[1] = g;
      px[2] = b;
      px[3] = a;
    }
  }
}

GlCursor::~GlCursor() = default;

bool GlCursor::EnsureGl() {
  if (gl_ready_) {
    return true;
  }
  if (gl_failed_) {
    return false;
  }

  const GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertexShader);
  const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragmentShader);
  if (vs == 0 || fs == 0) {
    if (vs != 0) {
      glDeleteShader(vs);
    }
    if (fs != 0) {
      glDeleteShader(fs);
    }
    gl_failed_ = true;
    return false;
  }

  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glLinkProgram(program_);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint linked = GL_FALSE;
  glGetProgramiv(program_, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    char log[512] = {};
    glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
    spdlog::error("[GlCursor] program link failed: {}", log);
    glDeleteProgram(program_);
    program_ = 0;
    gl_failed_ = true;
    return false;
  }

  a_pos_ = glGetAttribLocation(program_, "a_pos");
  a_uv_ = glGetAttribLocation(program_, "a_uv");
  u_tex_ = glGetUniformLocation(program_, "u_tex");
  if (a_pos_ < 0 || a_uv_ < 0 || u_tex_ < 0) {
    // A driver that dropped one of these would turn the GLuint casts below
    // into out-of-range attribute indices; bail instead.
    spdlog::error("[GlCursor] shader attribute/uniform location missing");
    glDeleteProgram(program_);
    program_ = 0;
    gl_failed_ = true;
    return false;
  }

  glGenTextures(1, &texture_);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(w_),
               static_cast<GLsizei>(h_), 0, GL_RGBA, GL_UNSIGNED_BYTE,
               rgba_.data());
  glBindTexture(GL_TEXTURE_2D, 0);

  gl_ready_ = true;
  return true;
}

void GlCursor::Draw(const uint32_t fb_w, const uint32_t fb_h) {
  if (!visible_.load(std::memory_order_acquire)) {
    return;
  }
  if (fb_w == 0 || fb_h == 0 || !EnsureGl()) {
    return;
  }

  const auto fw = static_cast<float>(fb_w);
  const auto fh = static_cast<float>(fb_h);
  const auto w = static_cast<float>(w_ * scale_);
  const auto h = static_cast<float>(h_ * scale_);
  const auto x0 = static_cast<float>(x_.load(std::memory_order_relaxed) -
                                     hot_x_ * static_cast<int32_t>(scale_));
  const auto y0 = static_cast<float>(y_.load(std::memory_order_relaxed) -
                                     hot_y_ * static_cast<int32_t>(scale_));
  const float x1 = x0 + w;
  const float y1 = y0 + h;

  // Pixel space (top-left origin, y-down) → NDC. The window framebuffer shows
  // the Flutter scene upright, so top-left pixel (0,0) maps to NDC (-1, +1).
  auto nx = [fw](const float x) { return x / fw * 2.0F - 1.0F; };
  auto ny = [fh](const float y) { return 1.0F - y / fh * 2.0F; };

  // Triangle strip: TL, BL, TR, BR. Texture V=0 is the top (arrow tip row).
  const std::array<float, 16> verts = {
      nx(x0), ny(y0), 0.0F, 0.0F,  // TL
      nx(x0), ny(y1), 0.0F, 1.0F,  // BL
      nx(x1), ny(y0), 1.0F, 0.0F,  // TR
      nx(x1), ny(y1), 1.0F, 1.0F,  // BR
  };

  // Flutter's renderer (Skia) caches GL state and does NOT re-query it between
  // frames. Any state we change and leave changed desyncs that cache and
  // corrupts the next frame (e.g. a disabled scissor → full-field flashing).
  // So snapshot every piece of state we touch and restore it exactly, leaving
  // the context byte-for-byte as Skia left it.
  GLint s_prog = 0;
  GLint s_fbo = 0;
  GLint s_vp[4] = {0, 0, 0, 0};
  GLint s_array_buf = 0;
  GLint s_active_tex = GL_TEXTURE0;
  GLint s_tex2d = 0;
  glGetIntegerv(GL_CURRENT_PROGRAM, &s_prog);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s_fbo);
  glGetIntegerv(GL_VIEWPORT, s_vp);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s_array_buf);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &s_active_tex);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &s_tex2d);

  const GLboolean s_blend = glIsEnabled(GL_BLEND);
  const GLboolean s_scissor = glIsEnabled(GL_SCISSOR_TEST);
  const GLboolean s_depth = glIsEnabled(GL_DEPTH_TEST);
  const GLboolean s_cull = glIsEnabled(GL_CULL_FACE);
  const GLboolean s_stencil = glIsEnabled(GL_STENCIL_TEST);
  GLint s_bsrc_rgb = GL_ONE;
  GLint s_bdst_rgb = GL_ZERO;
  GLint s_bsrc_a = GL_ONE;
  GLint s_bdst_a = GL_ZERO;
  GLint s_beq_rgb = GL_FUNC_ADD;
  GLint s_beq_a = GL_FUNC_ADD;
  glGetIntegerv(GL_BLEND_SRC_RGB, &s_bsrc_rgb);
  glGetIntegerv(GL_BLEND_DST_RGB, &s_bdst_rgb);
  glGetIntegerv(GL_BLEND_SRC_ALPHA, &s_bsrc_a);
  glGetIntegerv(GL_BLEND_DST_ALPHA, &s_bdst_a);
  glGetIntegerv(GL_BLEND_EQUATION_RGB, &s_beq_rgb);
  glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s_beq_a);

  struct AttribState {
    GLint enabled;
    GLint size;
    GLint type;
    GLint normalized;
    GLint stride;
    GLint buffer;
    void* pointer;
  };
  const std::array<GLint, 2> locs = {a_pos_, a_uv_};
  std::array<AttribState, 2> sa{};
  for (size_t i = 0; i < locs.size(); ++i) {
    const auto loc = static_cast<GLuint>(locs[i]);
    glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &sa[i].enabled);
    glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_SIZE, &sa[i].size);
    glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_TYPE, &sa[i].type);
    glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED,
                        &sa[i].normalized);
    glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &sa[i].stride);
    glGetVertexAttribiv(loc, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING,
                        &sa[i].buffer);
    glGetVertexAttribPointerv(loc, GL_VERTEX_ATTRIB_ARRAY_POINTER,
                              &sa[i].pointer);
  }

  // --- our draw ---
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, static_cast<GLsizei>(fb_w), static_cast<GLsizei>(fb_h));
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);  // premultiplied "over"

  glUseProgram(program_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture_);
  glUniform1i(u_tex_, 0);

  glBindBuffer(GL_ARRAY_BUFFER, 0);  // client-array vertex pointers
  glEnableVertexAttribArray(static_cast<GLuint>(a_pos_));
  glEnableVertexAttribArray(static_cast<GLuint>(a_uv_));
  glVertexAttribPointer(static_cast<GLuint>(a_pos_), 2, GL_FLOAT, GL_FALSE,
                        4 * sizeof(float), verts.data());
  glVertexAttribPointer(static_cast<GLuint>(a_uv_), 2, GL_FLOAT, GL_FALSE,
                        4 * sizeof(float), verts.data() + 2);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // --- restore exactly ---
  for (size_t i = 0; i < locs.size(); ++i) {
    const auto loc = static_cast<GLuint>(locs[i]);
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(sa[i].buffer));
    glVertexAttribPointer(loc, sa[i].size, static_cast<GLenum>(sa[i].type),
                          static_cast<GLboolean>(sa[i].normalized),
                          sa[i].stride, sa[i].pointer);
    if (sa[i].enabled) {
      glEnableVertexAttribArray(loc);
    } else {
      glDisableVertexAttribArray(loc);
    }
  }
  glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(s_array_buf));

  glBlendEquationSeparate(static_cast<GLenum>(s_beq_rgb),
                          static_cast<GLenum>(s_beq_a));
  glBlendFuncSeparate(
      static_cast<GLenum>(s_bsrc_rgb), static_cast<GLenum>(s_bdst_rgb),
      static_cast<GLenum>(s_bsrc_a), static_cast<GLenum>(s_bdst_a));

  auto set_enable = [](const GLenum cap, const GLboolean on) {
    if (on != GL_FALSE) {
      glEnable(cap);
    } else {
      glDisable(cap);
    }
  };
  set_enable(GL_BLEND, s_blend);
  set_enable(GL_SCISSOR_TEST, s_scissor);
  set_enable(GL_DEPTH_TEST, s_depth);
  set_enable(GL_CULL_FACE, s_cull);
  set_enable(GL_STENCIL_TEST, s_stencil);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(s_tex2d));
  glActiveTexture(static_cast<GLenum>(s_active_tex));
  glUseProgram(static_cast<GLuint>(s_prog));
  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(s_fbo));
  glViewport(s_vp[0], s_vp[1], s_vp[2], s_vp[3]);
}

void GlCursor::Destroy() {
  if (texture_ != 0) {
    glDeleteTextures(1, &texture_);
    texture_ = 0;
  }
  if (program_ != 0) {
    glDeleteProgram(program_);
    program_ = 0;
  }
  gl_ready_ = false;
}

}  // namespace homescreen
