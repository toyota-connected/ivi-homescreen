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

#include "backend/drm_kms_egl/gl_turn.h"

#include <array>

#include <GLES2/gl2ext.h>

#include "backend/drm_kms_egl/gl_state_guard.h"
#include "logging.h"

namespace homescreen {

namespace {

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
    ihs::log::error("[GlTurn] shader compile failed: {}", log);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

}  // namespace

bool GlTurn::EnsureProgram() {
  if (program_ != 0) {
    return true;
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
  a_pos_ = glGetAttribLocation(program_, "a_pos");
  a_uv_ = glGetAttribLocation(program_, "a_uv");
  u_tex_ = glGetUniformLocation(program_, "u_tex");
  if (linked != GL_TRUE || a_pos_ < 0 || a_uv_ < 0 || u_tex_ < 0) {
    char log[512] = {};
    glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
    ihs::log::error("[GlTurn] program link failed: {}", log);
    glDeleteProgram(program_);
    program_ = 0;
    return false;
  }
  return true;
}

bool GlTurn::Allocate(const uint32_t w, const uint32_t h) {
  const auto gw = static_cast<GLsizei>(w);
  const auto gh = static_cast<GLsizei>(h);
  GLint prev_fbo = 0;
  GLint prev_rb = 0;
  GLint prev_tex = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &prev_rb);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);

  if (fbo_ == 0) {
    glGenFramebuffers(1, &fbo_);
    glGenTextures(1, &color_);
    glGenRenderbuffers(1, &depth_stencil_);
  }
  glBindTexture(GL_TEXTURE_2D, color_);
  // Quarter turns land texels on pixels one to one, so nearest is exact.
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, gw, gh, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);

  glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         color_, 0);
  // Impeller takes a wrapped framebuffer to have depth and stencil, as the
  // window surface's EGL config does; Skia brings its own. Packed where the
  // driver has it, stencil alone otherwise, which is what Impeller clips with.
  glBindRenderbuffer(GL_RENDERBUFFER, depth_stencil_);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, gw, gh);
  bool packed = glGetError() == GL_NO_ERROR;
  if (packed) {
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, depth_stencil_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, depth_stencil_);
    packed =
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
  }
  if (!packed) {
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, 0);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, gw, gh);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, depth_stencil_);
  }
  const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

  glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prev_fbo));
  glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(prev_rb));
  glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(prev_tex));
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    ihs::log::error("[GlTurn] {}x{} framebuffer incomplete: 0x{:x}", w, h,
                    status);
    return false;
  }
  ihs::log::info("[GlTurn] engine renders into a {}x{} framebuffer ({})", w, h,
                 packed ? "depth24/stencil8" : "stencil8");
  return true;
}

GLuint GlTurn::Fbo(const uint32_t w, const uint32_t h) {
  if (failed_ || w == 0 || h == 0) {
    return 0;
  }
  if (ready_ && w == w_ && h == h_) {
    return fbo_;
  }
  ready_ = EnsureProgram() && Allocate(w, h);
  if (!ready_) {
    ihs::log::error(
        "[GlTurn] cannot render offscreen; the picture is not turned");
    failed_ = true;
    return 0;
  }
  w_ = w;
  h_ = h;
  return fbo_;
}

void GlTurn::Draw(const DisplayRotation& rotation) {
  if (!ready_ || rotation.panel_w <= 0 || rotation.panel_h <= 0) {
    return;
  }
  const auto pw = static_cast<float>(rotation.panel_w);
  const auto ph = static_cast<float>(rotation.panel_h);
  const auto rw = static_cast<double>(rotation.render_w);
  const auto rh = static_cast<double>(rotation.render_h);

  // A render corner on the panel, in NDC. Panel pixel (0, 0) is the top-left,
  // NDC (-1, +1).
  auto ndc = [&](const double x, const double y) {
    double px = 0;
    double py = 0;
    rotation.ToPanel(x, y, &px, &py);
    return std::array<float, 2>{static_cast<float>(px) / pw * 2.0F - 1.0F,
                                1.0F - static_cast<float>(py) / ph * 2.0F};
  };
  const auto tl = ndc(0, 0);
  const auto bl = ndc(0, rh);
  const auto tr = ndc(rw, 0);
  const auto br = ndc(rw, rh);
  // The engine renders a framebuffer bottom row first, as GL stores it, so
  // the picture's top row is at t = 1. Each vertex carries its render corner's
  // texel, and the turn of the quad turns the picture with it.
  const std::array<float, 16> verts = {
      tl[0], tl[1], 0.0F, 1.0F,  // render top-left
      bl[0], bl[1], 0.0F, 0.0F,  // render bottom-left
      tr[0], tr[1], 1.0F, 1.0F,  // render top-right
      br[0], br[1], 1.0F, 0.0F,  // render bottom-right
  };

  const ScopedGlState saved({a_pos_, a_uv_});

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, rotation.panel_w, rotation.panel_h);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  // Black around a picture smaller than the panel; the window surface's
  // buffers otherwise hold whatever they last showed.
  if (!rotation.fills_panel()) {
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
  }

  glUseProgram(program_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, color_);
  glUniform1i(u_tex_, 0);
  glBindBuffer(GL_ARRAY_BUFFER, 0);  // client-array vertex pointers
  glEnableVertexAttribArray(static_cast<GLuint>(a_pos_));
  glEnableVertexAttribArray(static_cast<GLuint>(a_uv_));
  glVertexAttribPointer(static_cast<GLuint>(a_pos_), 2, GL_FLOAT, GL_FALSE,
                        4 * sizeof(float), verts.data());
  glVertexAttribPointer(static_cast<GLuint>(a_uv_), 2, GL_FLOAT, GL_FALSE,
                        4 * sizeof(float), verts.data() + 2);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

void GlTurn::Destroy() {
  if (fbo_ != 0) {
    glDeleteFramebuffers(1, &fbo_);
    glDeleteTextures(1, &color_);
    glDeleteRenderbuffers(1, &depth_stencil_);
    fbo_ = color_ = depth_stencil_ = 0;
  }
  if (program_ != 0) {
    glDeleteProgram(program_);
    program_ = 0;
  }
  ready_ = false;
  w_ = h_ = 0;
}

}  // namespace homescreen
