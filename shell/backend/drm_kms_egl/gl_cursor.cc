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
#include <mutex>
#include <utility>
#include <vector>

#ifdef HAVE_DRM_CURSOR
#include <drm-cxx/cursor/cursor.hpp>
#include <drm-cxx/cursor/theme.hpp>
#endif

#include "backend/drm_kms_egl/gl_state_guard.h"
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
    ihs::log::error("[GlCursor] shader compile failed: {}", log);
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

bool GlCursor::SetShape(const char* const xcursor_name) {
#ifdef HAVE_DRM_CURSOR
  if (xcursor_name == nullptr) {
    SetVisible(false);
    return true;
  }
  // XCURSOR_SIZE convention is 24 logical px; the theme resolver scales to the
  // nearest available size.
  constexpr uint32_t kThemeLogicalSize = 24;
  static auto theme = drm::cursor::Theme::discover();
  if (!theme) {
    return false;
  }
  auto cursor =
      drm::cursor::Cursor::load(*theme, xcursor_name, {}, kThemeLogicalSize);
  if (!cursor) {
    return false;
  }
  const auto& f = cursor->first();
  // drm::cursor frames are premultiplied ARGB8888 (0xAARRGGBB); the GL texture
  // wants premultiplied RGBA bytes [R,G,B,A].
  const auto count = static_cast<size_t>(f.width) * f.height;
  std::vector<uint8_t> rgba(count * 4);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t px = f.pixels[i];
    rgba[i * 4 + 0] = static_cast<uint8_t>((px >> 16) & 0xFF);
    rgba[i * 4 + 1] = static_cast<uint8_t>((px >> 8) & 0xFF);
    rgba[i * 4 + 2] = static_cast<uint8_t>(px & 0xFF);
    rgba[i * 4 + 3] = static_cast<uint8_t>((px >> 24) & 0xFF);
  }
  {
    const std::lock_guard<std::mutex> lock(shape_mtx_);
    pending_rgba_ = std::move(rgba);
    pending_w_ = f.width;
    pending_h_ = f.height;
    pending_hot_x_ = f.xhot;
    pending_hot_y_ = f.yhot;
    shape_dirty_ = true;
  }
  SetVisible(true);
  return true;
#else
  (void)xcursor_name;
  return false;
#endif
}

bool GlCursor::SetImage(const uint32_t* const argb,
                        const uint32_t width,
                        const uint32_t height,
                        const int32_t hot_x,
                        const int32_t hot_y) {
  if (argb == nullptr || width == 0 || height == 0) {
    return false;
  }
  // Same conversion SetShape does on a themed frame: drm::cursor and this
  // interface both carry premultiplied 0xAARRGGBB, the GL texture wants
  // premultiplied [R,G,B,A].
  const auto count = static_cast<size_t>(width) * height;
  std::vector<uint8_t> rgba(count * 4);
  for (size_t i = 0; i < count; ++i) {
    const uint32_t px = argb[i];
    rgba[i * 4 + 0] = static_cast<uint8_t>((px >> 16) & 0xFF);
    rgba[i * 4 + 1] = static_cast<uint8_t>((px >> 8) & 0xFF);
    rgba[i * 4 + 2] = static_cast<uint8_t>(px & 0xFF);
    rgba[i * 4 + 3] = static_cast<uint8_t>((px >> 24) & 0xFF);
  }
  {
    const std::lock_guard<std::mutex> lock(shape_mtx_);
    pending_rgba_ = std::move(rgba);
    pending_w_ = width;
    pending_h_ = height;
    pending_hot_x_ = hot_x;
    pending_hot_y_ = hot_y;
    shape_dirty_ = true;
  }
  SetVisible(true);
  return true;
}

void GlCursor::ApplyPendingShape() {
  const std::lock_guard<std::mutex> lock(shape_mtx_);
  if (!shape_dirty_) {
    return;
  }
  rgba_ = std::move(pending_rgba_);
  w_ = pending_w_;
  h_ = pending_h_;
  hot_x_ = pending_hot_x_;
  hot_y_ = pending_hot_y_;
  // Themed cursors are full-size; drop the integer upscale that only exists to
  // make the tiny embedded fallback arrow visible.
  scale_ = 1;
  shape_dirty_ = false;
  if (texture_ != 0) {
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(w_),
                 static_cast<GLsizei>(h_), 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba_.data());
    glBindTexture(GL_TEXTURE_2D, 0);
  }
}

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
    ihs::log::error("[GlCursor] program link failed: {}", log);
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
    ihs::log::error("[GlCursor] shader attribute/uniform location missing");
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

void GlCursor::Draw(const uint32_t fb_w,
                    const uint32_t fb_h,
                    const DisplayRotation* rotation) {
  if (!visible_.load(std::memory_order_acquire)) {
    return;
  }
  if (fb_w == 0 || fb_h == 0 || !EnsureGl()) {
    return;
  }
  ApplyPendingShape();

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
  // A GPU-turned panel takes each corner through the turn first, which turns
  // the quad, and the sprite on it, with the picture.
  auto ndc = [fw, fh, rotation](const float x, const float y) {
    double px = x;
    double py = y;
    if (rotation != nullptr) {
      rotation->ToPanel(x, y, &px, &py);
    }
    return std::array<float, 2>{static_cast<float>(px) / fw * 2.0F - 1.0F,
                                1.0F - static_cast<float>(py) / fh * 2.0F};
  };
  const auto tl = ndc(x0, y0);
  const auto bl = ndc(x0, y1);
  const auto tr = ndc(x1, y0);
  const auto br = ndc(x1, y1);

  // Triangle strip: TL, BL, TR, BR. Texture V=0 is the top (arrow tip row).
  const std::array<float, 16> verts = {
      tl[0], tl[1], 0.0F, 0.0F,  // TL
      bl[0], bl[1], 0.0F, 1.0F,  // BL
      tr[0], tr[1], 1.0F, 0.0F,  // TR
      br[0], br[1], 1.0F, 1.0F,  // BR
  };

  // Put every piece of GL state the draw touches back as the engine left it.
  const ScopedGlState saved({a_pos_, a_uv_});

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
