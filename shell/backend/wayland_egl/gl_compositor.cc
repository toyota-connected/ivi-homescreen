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

#include "backend/wayland_egl/gl_compositor.h"

#include <array>

#include "backend/wayland_egl/gl_caps.h"
#include "logging.h"

namespace {

// GL_READ_FRAMEBUFFER / GL_DRAW_FRAMEBUFFER are ES3 core but the same
// constants are used for the NV/ANGLE extension variants.
constexpr GLenum kReadFramebuffer = 0x8CA8;
constexpr GLenum kDrawFramebuffer = 0x8CA9;

constexpr char kVertSrc[] =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = a_uv;\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

constexpr char kFragSrc[] =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

GLuint CompileShader(GLenum type, const char* src) {
  const GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetShaderiv(s, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) {
      glGetShaderInfoLog(s, len, nullptr, log.data());
    }
    spdlog::error("GlCompositor: shader compile failed: {}", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

}  // namespace

GlCompositor::GlCompositor(const GlCaps* caps) : caps_(caps) {}

GlCompositor::~GlCompositor() {
  if (vbo_) {
    glDeleteBuffers(1, &vbo_);
  }
  if (program_) {
    glDeleteProgram(program_);
  }
}

bool GlCompositor::EnsureQuad() {
  if (quad_initialized_) {
    return program_ != 0;
  }
  quad_initialized_ = true;

  const GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertSrc);
  const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragSrc);
  if (!vs || !fs) {
    if (vs)
      glDeleteShader(vs);
    if (fs)
      glDeleteShader(fs);
    return false;
  }

  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glLinkProgram(program_);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint ok = 0;
  glGetProgramiv(program_, GL_LINK_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) {
      glGetProgramInfoLog(program_, len, nullptr, log.data());
    }
    spdlog::error("GlCompositor: program link failed: {}", log);
    glDeleteProgram(program_);
    program_ = 0;
    return false;
  }

  attr_pos_ = glGetAttribLocation(program_, "a_pos");
  attr_uv_ = glGetAttribLocation(program_, "a_uv");
  uni_tex_ = glGetUniformLocation(program_, "u_tex");

  // Fullscreen triangle strip in NDC: (x, y, u, v) per vertex.
  constexpr std::array<GLfloat, 16> verts = {{
      -1.f,
      -1.f,
      0.f,
      0.f,
      1.f,
      -1.f,
      1.f,
      0.f,
      -1.f,
      1.f,
      0.f,
      1.f,
      1.f,
      1.f,
      1.f,
      1.f,
  }};
  glGenBuffers(1, &vbo_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts.data(), GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  return true;
}

void GlCompositor::CompositeViaQuad(GLuint src_color_tex,
                                    GLint dst_x,
                                    GLint dst_y,
                                    GLsizei dst_w,
                                    GLsizei dst_h) {
  if (!EnsureQuad()) {
    return;
  }

  // Snapshot minimal state: we don't implement full state save/restore,
  // but we set everything we rely on so the engine's next render isn't
  // surprised. Flutter resets its own GL state on entry.
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);

  glViewport(dst_x, dst_y, dst_w, dst_h);

  glUseProgram(program_);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, src_color_tex);
  if (uni_tex_ >= 0) {
    glUniform1i(uni_tex_, 0);
  }

  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  if (attr_pos_ >= 0) {
    glEnableVertexAttribArray(static_cast<GLuint>(attr_pos_));
    glVertexAttribPointer(static_cast<GLuint>(attr_pos_), 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), nullptr);
  }
  if (attr_uv_ >= 0) {
    glEnableVertexAttribArray(static_cast<GLuint>(attr_uv_));
    // glVertexAttribPointer takes the buffer offset as a void* by historic
    // GL convention — there's no arithmetic on the pointer. Suppress the
    // "integer to pointer cast" lint.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    glVertexAttribPointer(static_cast<GLuint>(attr_uv_), 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat),
                          reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
  }

  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  if (attr_pos_ >= 0) {
    glDisableVertexAttribArray(static_cast<GLuint>(attr_pos_));
  }
  if (attr_uv_ >= 0) {
    glDisableVertexAttribArray(static_cast<GLuint>(attr_uv_));
  }
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
}

void GlCompositor::CompositeToDefault(GLuint src_fbo,
                                      GLuint src_color_tex,
                                      GLsizei src_w,
                                      GLsizei src_h,
                                      GLint dst_x,
                                      GLint dst_y,
                                      GLsizei dst_w,
                                      GLsizei dst_h) {
  if (caps_ && caps_->has_blit_framebuffer && caps_->blit_framebuffer) {
    glBindFramebuffer(kReadFramebuffer, src_fbo);
    glBindFramebuffer(kDrawFramebuffer, 0);
    caps_->blit_framebuffer(0, 0, src_w, src_h, dst_x, dst_y, dst_x + dst_w,
                            dst_y + dst_h, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return;
  }
  CompositeViaQuad(src_color_tex, dst_x, dst_y, dst_w, dst_h);
}
