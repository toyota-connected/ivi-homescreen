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
#include "logging/logging.h"

#include <array>
#include <cstring>
#include <string>
#include <string_view>

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
    "uniform float u_uv_y_scale;\n"
    "uniform float u_uv_y_offset;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  v_uv = vec2(a_uv.x, a_uv.y * u_uv_y_scale + u_uv_y_offset);\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

constexpr char kFragSrc[] =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

// The same quad, sampling an external image. A dma-buf holding planar YUV can
// only be sampled through samplerExternalOES, which is what applies the
// colour-space conversion; the driver picks BT.601/709 from the image. Built
// only when the context advertises the extension, so a driver without it keeps
// working for packed formats.
constexpr char kFragSrcExternal[] =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform samplerExternalOES u_tex;\n"
    "void main() {\n"
    "  gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

// Exact, space-delimited match. A substring search would accept
// GL_OES_EGL_image_external_essl3 as the base extension and go on to compile a
// shader the driver cannot link.
bool ExtensionSupported(const char* extensions, const char* name) {
  if (extensions == nullptr || name == nullptr) {
    return false;
  }
  const size_t name_len = std::strlen(name);
  const std::string_view sv(extensions);
  size_t pos = 0;
  while ((pos = sv.find(name, pos)) != std::string_view::npos) {
    const bool left_ok = (pos == 0) || (sv[pos - 1] == ' ');
    const size_t end = pos + name_len;
    const bool right_ok = (end == sv.size()) || (sv[end] == ' ');
    if (left_ok && right_ok) {
      return true;
    }
    pos = end;
  }
  return false;
}

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
    ihs::log::error("GlCompositor: shader compile failed: {}", log);
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
  if (program_external_) {
    glDeleteProgram(program_external_);
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
    ihs::log::error("GlCompositor: program link failed: {}", log);
    glDeleteProgram(program_);
    program_ = 0;
    return false;
  }

  attr_pos_ = glGetAttribLocation(program_, "a_pos");
  attr_uv_ = glGetAttribLocation(program_, "a_uv");
  uni_tex_ = glGetUniformLocation(program_, "u_tex");
  BuildExternalProgram();
  uni_uv_y_scale_ = glGetUniformLocation(program_, "u_uv_y_scale");
  uni_uv_y_offset_ = glGetUniformLocation(program_, "u_uv_y_offset");

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

void GlCompositor::BuildExternalProgram() {
  // Optional: a driver without the extension keeps the packed-format path, and
  // a planar frame is then simply not composited rather than mis-sampled.
  const auto* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (!ExtensionSupported(exts, "GL_OES_EGL_image_external")) {
    ihs::log::warn(
        "GlCompositor: GL_OES_EGL_image_external absent; planar YUV "
        "platform-view frames cannot be sampled");
    return;
  }
  const GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertSrc);
  const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragSrcExternal);
  if (!vs || !fs) {
    if (vs) {
      glDeleteShader(vs);
    }
    if (fs) {
      glDeleteShader(fs);
    }
    return;
  }
  program_external_ = glCreateProgram();
  glAttachShader(program_external_, vs);
  glAttachShader(program_external_, fs);
  glLinkProgram(program_external_);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(program_external_, GL_LINK_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetProgramiv(program_external_, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) {
      glGetProgramInfoLog(program_external_, len, nullptr, log.data());
    }
    ihs::log::error("GlCompositor: external program link failed: {}", log);
    glDeleteProgram(program_external_);
    program_external_ = 0;
    return;
  }
  attr_pos_external_ = glGetAttribLocation(program_external_, "a_pos");
  attr_uv_external_ = glGetAttribLocation(program_external_, "a_uv");
  uni_tex_external_ = glGetUniformLocation(program_external_, "u_tex");
  uni_uv_y_scale_external_ =
      glGetUniformLocation(program_external_, "u_uv_y_scale");
  uni_uv_y_offset_external_ =
      glGetUniformLocation(program_external_, "u_uv_y_offset");
}

void GlCompositor::EmitPersistentQuadState() {
  // Once-per-frame setup. Establishes a known baseline (blend disabled,
  // bound_tex_ cleared) so per-call deltas can be tracked accurately.
  glDisable(GL_BLEND);
  blend_enabled_ = false;
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);

  glUseProgram(program_);
  glActiveTexture(GL_TEXTURE0);
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
  bound_tex_ = 0;
}

void GlCompositor::TearDownPersistentQuadState() {
  if (blend_enabled_) {
    glDisable(GL_BLEND);
    blend_enabled_ = false;
  }
  if (attr_pos_ >= 0) {
    glDisableVertexAttribArray(static_cast<GLuint>(attr_pos_));
  }
  if (attr_uv_ >= 0) {
    glDisableVertexAttribArray(static_cast<GLuint>(attr_uv_));
  }
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindTexture(GL_TEXTURE_2D, 0);
  glUseProgram(0);
  bound_tex_ = 0;
}

void GlCompositor::CompositeViaQuadExternal(GLuint tex,
                                            GLint dst_x,
                                            GLint dst_y,
                                            GLsizei dst_w,
                                            GLsizei dst_h,
                                            bool blend,
                                            bool flip_y) {
  // Self-contained: this program has its own attribute and uniform locations,
  // so it cannot ride the batched state emitted for the sampler2D program.
  if (persistent_state_emitted_) {
    TearDownPersistentQuadState();
    persistent_state_emitted_ = false;
  }
  // Same baseline the batched path establishes: anything left enabled by the
  // caller could clip or discard this draw.
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);

  glUseProgram(program_external_);
  glActiveTexture(GL_TEXTURE0);
  if (uni_tex_external_ >= 0) {
    glUniform1i(uni_tex_external_, 0);
  }
  if (uni_uv_y_scale_external_ >= 0) {
    glUniform1f(uni_uv_y_scale_external_, flip_y ? -1.f : 1.f);
  }
  if (uni_uv_y_offset_external_ >= 0) {
    glUniform1f(uni_uv_y_offset_external_, flip_y ? 1.f : 0.f);
  }
  if (blend) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  } else {
    glDisable(GL_BLEND);
  }
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  if (attr_pos_external_ >= 0) {
    glEnableVertexAttribArray(static_cast<GLuint>(attr_pos_external_));
    glVertexAttribPointer(static_cast<GLuint>(attr_pos_external_), 2, GL_FLOAT,
                          GL_FALSE, 4 * sizeof(GLfloat), nullptr);
  }
  if (attr_uv_external_ >= 0) {
    glEnableVertexAttribArray(static_cast<GLuint>(attr_uv_external_));
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    glVertexAttribPointer(static_cast<GLuint>(attr_uv_external_), 2, GL_FLOAT,
                          GL_FALSE, 4 * sizeof(GLfloat),
                          reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
  }
  glViewport(dst_x, dst_y, dst_w, dst_h);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
  if (attr_pos_external_ >= 0) {
    glDisableVertexAttribArray(static_cast<GLuint>(attr_pos_external_));
  }
  if (attr_uv_external_ >= 0) {
    glDisableVertexAttribArray(static_cast<GLuint>(attr_uv_external_));
  }
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glUseProgram(0);
  // Blend is left off rather than as this draw wanted it. This path clears
  // persistent_state_emitted_, so EndFrame will not tear anything down, and an
  // enabled blend would leak into the rest of the present and the next frame.
  if (blend) {
    glDisable(GL_BLEND);
  }
  blend_enabled_ = false;
  bound_tex_ = 0;
}

void GlCompositor::BeginFrame() {
  frame_open_ = true;
  persistent_state_emitted_ = false;
}

void GlCompositor::EndFrame() {
  if (persistent_state_emitted_) {
    TearDownPersistentQuadState();
    persistent_state_emitted_ = false;
  }
  frame_open_ = false;
}

void GlCompositor::CompositeViaQuad(GLuint src_color_tex,
                                    GLint dst_x,
                                    GLint dst_y,
                                    GLsizei dst_w,
                                    GLsizei dst_h,
                                    bool blend,
                                    bool flip_y,
                                    bool external) {
  if (!EnsureQuad()) {
    return;
  }
  if (external && program_external_ == 0) {
    return;  // no external sampler; better blank than the luma plane in red
  }
  // The external path is rare (one platform-view layer), so it does not join
  // the frame-batched fast path: it sets its own program and state and leaves
  // the batched baseline to be re-emitted for the next 2D layer.
  if (external) {
    CompositeViaQuadExternal(src_color_tex, dst_x, dst_y, dst_w, dst_h, blend,
                             flip_y);
    return;
  }

  // Frame-batched fast path: emit the persistent state once on the first
  // quad call inside Begin/EndFrame and only the changed delta thereafter.
  // Every other layer's composite drops from ~25 GL calls to ~5.
  if (frame_open_) {
    if (!persistent_state_emitted_) {
      EmitPersistentQuadState();
      persistent_state_emitted_ = true;
    }
    if (blend && !blend_enabled_) {
      // Flutter / Skia backing stores are premultiplied alpha.
      glEnable(GL_BLEND);
      glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
      blend_enabled_ = true;
    } else if (!blend && blend_enabled_) {
      glDisable(GL_BLEND);
      blend_enabled_ = false;
    }
    glViewport(dst_x, dst_y, dst_w, dst_h);
    if (bound_tex_ != src_color_tex) {
      glBindTexture(GL_TEXTURE_2D, src_color_tex);
      bound_tex_ = src_color_tex;
    }
    if (uni_uv_y_scale_ >= 0) {
      glUniform1f(uni_uv_y_scale_, flip_y ? -1.0f : 1.0f);
    }
    if (uni_uv_y_offset_ >= 0) {
      glUniform1f(uni_uv_y_offset_, flip_y ? 1.0f : 0.0f);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    return;
  }

  // One-off path (no Begin/End wrapping the call): full setup + teardown
  // per call so the engine's next render isn't surprised by leftover state.
  if (blend) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  } else {
    glDisable(GL_BLEND);
  }
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
  if (uni_uv_y_scale_ >= 0) {
    glUniform1f(uni_uv_y_scale_, flip_y ? -1.0f : 1.0f);
  }
  if (uni_uv_y_offset_ >= 0) {
    glUniform1f(uni_uv_y_offset_, flip_y ? 1.0f : 0.0f);
  }

  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  if (attr_pos_ >= 0) {
    glEnableVertexAttribArray(static_cast<GLuint>(attr_pos_));
    glVertexAttribPointer(static_cast<GLuint>(attr_pos_), 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat), nullptr);
  }
  if (attr_uv_ >= 0) {
    glEnableVertexAttribArray(static_cast<GLuint>(attr_uv_));
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

void GlCompositor::CompositeToFbo(GLuint dst_fbo,
                                  GLuint src_fbo,
                                  GLuint src_color_tex,
                                  GLsizei src_w,
                                  GLsizei src_h,
                                  GLint dst_x,
                                  GLint dst_y,
                                  GLsizei dst_w,
                                  GLsizei dst_h,
                                  bool blend,
                                  bool flip_y,
                                  bool external) {
  // Blit can't alpha-blend, so overlay layers must take the quad path.
  // Also skip blit when there is no source FBO to read from (e.g. raw
  // platform-view textures with no associated FBO) — falling through to
  // the quad path handles that case via sampler.
  // glBlitFramebuffer flips vertically when dstY1 < dstY0; we use that for
  // GL-native-origin sources that need to land top-down.
  // An external texture has no FBO to read from and must go through its own
  // sampler, so it never takes the blit path.
  if (!external && !blend && src_fbo != 0 && caps_ &&
      caps_->has_blit_framebuffer && caps_->blit_framebuffer) {
    glBindFramebuffer(kReadFramebuffer, src_fbo);
    glBindFramebuffer(kDrawFramebuffer, dst_fbo);
    const GLint dst_y0 = flip_y ? dst_y + dst_h : dst_y;
    const GLint dst_y1 = flip_y ? dst_y : dst_y + dst_h;
    caps_->blit_framebuffer(0, 0, src_w, src_h, dst_x, dst_y0, dst_x + dst_w,
                            dst_y1, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return;
  }
  // Quad path draws into whatever FBO is currently bound.
  glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
  CompositeViaQuad(src_color_tex, dst_x, dst_y, dst_w, dst_h, blend, flip_y,
                   external);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
