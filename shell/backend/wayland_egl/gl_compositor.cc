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
#include "backend/gl_extensions.h"
#include "logging/logging.h"

#include <GLES2/gl2ext.h>  // GL_TEXTURE_EXTERNAL_OES

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "backend/wayland_egl/gl_caps.h"
#include "logging.h"

namespace {

// GL_READ_FRAMEBUFFER / GL_DRAW_FRAMEBUFFER are ES3 core but the same
// constants are used for the NV/ANGLE extension variants.
constexpr GLenum kReadFramebuffer = 0x8CA8;
constexpr GLenum kDrawFramebuffer = 0x8CA9;

// a_uv is (0, 0) at the viewport's bottom-left. (s, t) is the same point with
// t running top to bottom, which is the space UvAffine is written in, so a
// source crop and a buffer transform are one affine away.
//
// Every shader here is built twice, with IHS_MASK 0 and 1 (ShaderSource). The
// masked build cuts a quad's corners to a rounded clip and draws quads given
// vertex by vertex (QuadMask): the pieces of a rounded layer, and the quad of
// a transformed view. The other has none of that code, so a quad without
// either pays nothing for it.
//
// A vertex of the masked build carries its homogeneous w (a_w), 1 except
// under a perspective. Its position is given already divided by w, and the
// varyings are (s, t, 1) / w, which are affine on the screen: the fragment
// divides them back out, so (s, t) are exact at every pixel, and the texture
// coordinates (u_uv_*) and the point in the clip's space (u_pos_*, given over
// w0 and scaled back by 1 / w the same way) are worked out there. Not left to
// the GPU's perspective-correct interpolation: v3d's GLES interpolated these
// affinely when the w was in gl_Position.
//
// a_edge is the vertex's distance in pixels to each of the quad's edges
// (s = 0, s = 1, t = 0, t = 1), above 0 inside. The edges are straight on the
// screen, so the distances are affine there and interpolate exactly. An edge
// left hard is given far away (kNoEdge).
constexpr char kVertSrc[] =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "#if IHS_MASK\n"
    "attribute float a_mask;\n"
    "attribute float a_w;\n"
    "attribute vec4 a_edge;\n"
    "varying vec3 v_stq;\n"
    "varying float v_mask;\n"
    "varying vec4 v_edge;\n"
    "#else\n"
    "uniform vec3 u_uv_u;\n"
    "uniform vec3 u_uv_v;\n"
    "varying vec2 v_uv;\n"
    "#endif\n"
    "void main() {\n"
    "  vec3 st = vec3(a_uv.x, 1.0 - a_uv.y, 1.0);\n"
    "#if IHS_MASK\n"
    "  v_stq = st / a_w;\n"
    "  v_mask = a_mask;\n"
    "  v_edge = a_edge;\n"
    "#else\n"
    "  v_uv = vec2(dot(u_uv_u, st), dot(u_uv_v, st));\n"
    "#endif\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

constexpr char kFragPrecision[] = "precision mediump float;\n";

// Uv() is the pixel's texture coordinates, with (s, t) held to u_st_clamp
// (SampleClamp): past a smoothed edge the quad is drawn a pixel wider, and
// what is drawn there is the edge's own texels, not the buffer's beyond a
// crop.
//
// Coverage() is the share of the pixel the view shows. The shares inside the
// quad's smoothed edges, half a pixel either side of each, multiply, which is
// right at its corners. A clip's share is taken as the least of it and
// theirs, which is right where the clip runs along an edge of the view, as a
// clip around the view does, and near enough where one crosses it. Round() is
// the share inside a rounded rect in output pixels, for a pixel the quad's
// crop has already put inside its bounding box: only the corners are left to
// cut, with about a pixel of edge. v_mask is 0 on the pieces of the quad no
// corner reaches (QuadMask::parts), which skip it. A corner with a zero
// radius is square. Clips() is the share inside clips drawn as shapes
// (ClipShapeDistance in view/layer_geometry.h, worked out the same way), up
// to IHS_CLIPS of them, as many as the fragment stage has uniforms for.
// Positions are in pixels, up to a few thousand, which mediump cannot place
// to a pixel, so the masked build works in highp where the GPU has it.
// Without the mask, Coverage() is 1.
constexpr char kMaskSrc[] =
    "#if IHS_MASK\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "#define MASKP highp\n"
    "#else\n"
    "#define MASKP mediump\n"
    "#endif\n"
    "varying MASKP vec3 v_stq;\n"
    "varying float v_mask;\n"
    "varying MASKP vec4 v_edge;\n"
    "uniform MASKP vec4 u_st_clamp;\n"  // s0, t0, s1, t1
    "uniform MASKP vec3 u_uv_u;\n"
    "uniform MASKP vec3 u_uv_v;\n"
    "uniform MASKP vec3 u_pos_u;\n"
    "uniform MASKP vec3 u_pos_v;\n"
    "uniform MASKP vec4 u_rrect;\n"         // left, top, right, bottom
    "uniform MASKP vec4 u_radii_top;\n"     // upper left x y, upper right x y
    "uniform MASKP vec4 u_radii_bottom;\n"  // lower left x y, lower right x y
    "MASKP vec3 St() { return vec3(v_stq.xy / v_stq.z, 1.0); }\n"
    "MASKP vec2 Uv() {\n"
    "  MASKP vec3 st = vec3(clamp(St().xy, u_st_clamp.xy, u_st_clamp.zw),\n"
    "                       1.0);\n"
    "  return vec2(dot(u_uv_u, st), dot(u_uv_v, st));\n"
    "}\n"
    "MASKP vec2 Pos() {\n"
    "  MASKP vec3 st = St();\n"
    "  return vec2(dot(u_pos_u, st), dot(u_pos_v, st)) * v_stq.z;\n"
    "}\n"
    "float Round() {\n"
    "  if (v_mask < 0.5) return 1.0;\n"
    "  MASKP vec2 p = Pos();\n"
    "  bool left = p.x < 0.5 * (u_rrect.x + u_rrect.z);\n"
    "  bool top = p.y < 0.5 * (u_rrect.y + u_rrect.w);\n"
    "  MASKP vec4 pair = top ? u_radii_top : u_radii_bottom;\n"
    "  MASKP vec2 r = left ? pair.xy : pair.zw;\n"
    "  if (r.x <= 0.0 || r.y <= 0.0) return 1.0;\n"
    "  MASKP vec2 d = p - vec2(left ? u_rrect.x + r.x : u_rrect.z - r.x,\n"
    "                          top ? u_rrect.y + r.y : u_rrect.w - r.y);\n"
    "  if ((left ? d.x >= 0.0 : d.x <= 0.0) ||\n"
    "      (top ? d.y >= 0.0 : d.y <= 0.0)) return 1.0;\n"
    "  MASKP float k = length(d / r);\n"
    "  return clamp(0.5 - (k - 1.0) * min(r.x, r.y), 0.0, 1.0);\n"
    "}\n"
    "#if IHS_CLIPS > 0\n"
    "uniform MASKP vec4 u_clip_map[3 * IHS_CLIPS];\n"  // x, y, w rows; unit
    "uniform MASKP vec4 u_clip_rect[IHS_CLIPS];\n"     // left top right bottom
    "uniform MASKP vec4 u_clip_radii[2 * IHS_CLIPS];\n"  // as u_radii_*
    "uniform int u_clip_count;\n"
    "float Clips() {\n"
    "  float cover = 1.0;\n"
    "  MASKP vec3 p = vec3(Pos(), 1.0);\n"
    "  for (int k = 0; k < IHS_CLIPS; ++k) {\n"
    "    if (k >= u_clip_count) break;\n"
    "    MASKP vec4 mx4 = u_clip_map[3 * k];\n"
    "    MASKP vec3 mx = mx4.xyz;\n"
    "    MASKP vec3 my = u_clip_map[3 * k + 1].xyz;\n"
    "    MASKP vec3 mw = u_clip_map[3 * k + 2].xyz;\n"
    "    MASKP float w = dot(mw, p);\n"
    "    if (w <= 0.0) return 0.0;\n"
    "    MASKP vec2 q = vec2(dot(mx, p), dot(my, p)) / w;\n"
    "    MASKP vec4 b = u_clip_rect[k];\n"
    "    MASKP vec4 top2 = u_clip_radii[2 * k];\n"
    "    MASKP vec4 bottom2 = u_clip_radii[2 * k + 1];\n"
    // More than a pixel (mx4.w) inside the straight parts: wholly inside.
    "    MASKP float m = mx4.w;\n"
    "    MASKP vec2 lo = b.xy + m;\n"
    "    MASKP vec2 hi = b.zw - m;\n"
    "    MASKP vec2 lo_r = lo + vec2(max(top2.x, bottom2.x),\n"
    "                                max(top2.y, top2.w));\n"
    "    MASKP vec2 hi_r = hi - vec2(max(top2.z, bottom2.z),\n"
    "                                max(bottom2.y, bottom2.w));\n"
    "    if ((q.x > lo.x && q.x < hi.x && q.y > lo_r.y && q.y < hi_r.y) ||\n"
    "        (q.y > lo.y && q.y < hi.y && q.x > lo_r.x && q.x < hi_r.x)) {\n"
    "      continue;\n"
    "    }\n"
    "    MASKP vec2 jx = (vec2(mx.x, my.x) - q * mw.x) / w;\n"
    "    MASKP vec2 jy = (vec2(mx.y, my.y) - q * mw.y) / w;\n"
    "    bool left = q.x < 0.5 * (b.x + b.z);\n"
    "    bool top = q.y < 0.5 * (b.y + b.w);\n"
    "    MASKP vec4 pair = top ? top2 : bottom2;\n"
    "    MASKP vec2 r = left ? pair.xy : pair.zw;\n"
    "    MASKP vec2 d = q - vec2(left ? b.x + r.x : b.z - r.x,\n"
    "                            top ? b.y + r.y : b.w - r.y);\n"
    "    MASKP float f;\n"
    "    MASKP vec2 n;\n"
    "    if (r.x > 0.0 && r.y > 0.0 && (left ? d.x < 0.0 : d.x > 0.0) &&\n"
    "        (top ? d.y < 0.0 : d.y > 0.0)) {\n"
    "      MASKP float e = length(d / r);\n"
    "      f = e - 1.0;\n"
    "      n = d / (r * r * e);\n"
    "    } else {\n"
    "      MASKP vec2 e = abs(q - 0.5 * (b.xy + b.zw)) - 0.5 * (b.zw - b.xy);\n"
    "      if (e.x > e.y) {\n"
    "        f = e.x;\n"
    "        n = vec2(left ? -1.0 : 1.0, 0.0);\n"
    "      } else {\n"
    "        f = e.y;\n"
    "        n = vec2(0.0, top ? -1.0 : 1.0);\n"
    "      }\n"
    "    }\n"
    "    MASKP float rate = max(length(vec2(dot(jx, n), dot(jy, n))), 1e-6);\n"
    "    cover = min(cover, clamp(0.5 - f / rate, 0.0, 1.0));\n"
    "    if (cover <= 0.0) break;\n"
    "  }\n"
    "  return cover;\n"
    "}\n"
    "#else\n"
    "float Clips() { return 1.0; }\n"
    "#endif\n"
    "float Coverage() {\n"
    "  MASKP vec4 e = clamp(v_edge + 0.5, 0.0, 1.0);\n"
    "  float edges = e.x * e.y * e.z * e.w;\n"
    "  if (edges <= 0.0 || v_mask < 0.5) return edges;\n"
    "  return min(edges, min(Round(), Clips()));\n"
    "}\n"
    "#else\n"
    "varying vec2 v_uv;\n"
    "vec2 Uv() { return v_uv; }\n"
    "float Coverage() { return 1.0; }\n"
    "#endif\n";

// u_opaque forces alpha to 1: an XRGB buffer's alpha is undefined, and
// premultiplied color is unchanged by it. u_alpha is the view's opacity; the
// color is premultiplied, so it and the mask's coverage scale all four
// channels.
constexpr char kFragBody[] =
    "uniform sampler2D u_tex;\n"
    "uniform float u_opaque;\n"
    "uniform float u_alpha;\n"
    "void main() {\n"
    "  vec4 c = texture2D(u_tex, Uv());\n"
    "  gl_FragColor =\n"
    "      vec4(c.rgb, mix(c.a, 1.0, u_opaque)) * (u_alpha * Coverage());\n"
    "}\n";

// The same quad, sampling an external image. A dma-buf holding planar YUV can
// only be sampled through samplerExternalOES, which is what applies the
// color-space conversion; the driver picks BT.601/709 from the image. Built
// only when the context advertises the extension, so a driver without it keeps
// working for packed formats.
constexpr char kFragExtension[] =
    "#extension GL_OES_EGL_image_external : require\n";
constexpr char kFragBodyExternal[] =
    "uniform samplerExternalOES u_tex;\n"
    "uniform float u_opaque;\n"
    "uniform float u_alpha;\n"
    "void main() {\n"
    "  vec4 c = texture2D(u_tex, Uv());\n"
    "  gl_FragColor =\n"
    "      vec4(c.rgb, mix(c.a, 1.0, u_opaque)) * (u_alpha * Coverage());\n"
    "}\n";

// The vertex and fragment source of the quad program: external or 2D, with or
// without the rounded-clip mask.
std::string VertSource(const bool mask) {
  return std::string("#define IHS_MASK ") + (mask ? "1" : "0") + "\n" +
         kVertSrc;
}
std::string FragSource(const bool external, const bool mask, const int clips) {
  return std::string(external ? kFragExtension : "") + "#define IHS_MASK " +
         (mask ? "1" : "0") + "\n#define IHS_CLIPS " + std::to_string(clips) +
         "\n" + kFragPrecision + kMaskSrc +
         (external ? kFragBodyExternal : kFragBody);
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
  if (parts_vbo_) {
    glDeleteBuffers(1, &parts_vbo_);
  }
  if (quad_vao_ != 0 && caps_ != nullptr &&
      caps_->delete_vertex_arrays != nullptr) {
    caps_->delete_vertex_arrays(1, &quad_vao_);
  }
  if (program_) {
    glDeleteProgram(program_);
  }
  for (const QuadProgram* p : {&external_, &mask_, &external_mask_}) {
    if (p->id != 0) {
      glDeleteProgram(p->id);
    }
  }
}

bool GlCompositor::EnsureQuad() {
  if (quad_initialized_) {
    return program_ != 0;
  }
  quad_initialized_ = true;

  const GLuint vs =
      CompileShader(GL_VERTEX_SHADER, VertSource(/*mask=*/false).c_str());
  const GLuint fs =
      CompileShader(GL_FRAGMENT_SHADER,
                    FragSource(/*external=*/false, /*mask=*/false, 0).c_str());
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
  BuildSelfContainedPrograms();
  uni_uv_u_ = glGetUniformLocation(program_, "u_uv_u");
  uni_uv_v_ = glGetUniformLocation(program_, "u_uv_v");
  uni_opaque_ = glGetUniformLocation(program_, "u_opaque");
  uni_alpha_ = glGetUniformLocation(program_, "u_alpha");

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

void GlCompositor::BuildSelfContainedPrograms() {
  // Clip shapes take six uniform vectors each, on top of the eleven the
  // masked build has; as many as the fragment stage holds, up to the most a
  // view keeps. ES2 promises only 16, which leaves none.
  GLint vectors = 0;
  glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &vectors);
  clip_slots_ = static_cast<size_t>(
      std::clamp((vectors - 12) / 6, 0, static_cast<int>(ClipShapes::kMax)));
  mask_ = BuildQuadProgram(/*external=*/false, /*mask=*/true, clip_slots_);
  if (mask_.id != 0 && clip_slots_ > 0 && mask_.mask.clip_count < 0) {
    clip_slots_ = 0;  // optimized away: not there to set
  }
  // Optional: a driver without the extension keeps the packed-format path, and
  // a planar frame is then simply not composited rather than mis-sampled.
  const auto* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (!ihs::gl::ExtensionSupported(exts, "GL_OES_EGL_image_external")) {
    // Left unbuilt, not an error: a context that never presents a YUV
    // platform-view frame does not need it. The one warning, if a frame does
    // arrive, is emitted where the composite is refused.
    return;
  }
  external_ = BuildQuadProgram(/*external=*/true, /*mask=*/false, 0);
  if (external_.id != 0) {
    external_mask_ =
        BuildQuadProgram(/*external=*/true, /*mask=*/true, clip_slots_);
  }
}

GlCompositor::QuadProgram GlCompositor::BuildQuadProgram(const bool external,
                                                         const bool mask,
                                                         const size_t clips) {
  QuadProgram p;
  const GLuint vs = CompileShader(GL_VERTEX_SHADER, VertSource(mask).c_str());
  const GLuint fs = CompileShader(
      GL_FRAGMENT_SHADER,
      FragSource(external, mask, mask ? static_cast<int>(clips) : 0).c_str());
  if (!vs || !fs) {
    if (vs) {
      glDeleteShader(vs);
    }
    if (fs) {
      glDeleteShader(fs);
    }
    return p;
  }
  p.id = glCreateProgram();
  glAttachShader(p.id, vs);
  glAttachShader(p.id, fs);
  glLinkProgram(p.id);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
  if (!ok) {
    GLint len = 0;
    glGetProgramiv(p.id, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<size_t>(len > 0 ? len : 0), '\0');
    if (len > 0) {
      glGetProgramInfoLog(p.id, len, nullptr, log.data());
    }
    ihs::log::error("GlCompositor: {}{} program link failed: {}",
                    external ? "external" : "2D", mask ? " masked" : "", log);
    glDeleteProgram(p.id);
    p.id = 0;
    return p;
  }
  p.attr_pos = glGetAttribLocation(p.id, "a_pos");
  p.attr_uv = glGetAttribLocation(p.id, "a_uv");
  p.attr_mask = glGetAttribLocation(p.id, "a_mask");
  p.attr_w = glGetAttribLocation(p.id, "a_w");
  p.attr_edge = glGetAttribLocation(p.id, "a_edge");
  p.uni_tex = glGetUniformLocation(p.id, "u_tex");
  p.uni_uv_u = glGetUniformLocation(p.id, "u_uv_u");
  p.uni_uv_v = glGetUniformLocation(p.id, "u_uv_v");
  p.uni_opaque = glGetUniformLocation(p.id, "u_opaque");
  p.uni_alpha = glGetUniformLocation(p.id, "u_alpha");
  if (mask) {
    p.mask = GetMaskUniforms(p.id);
  }
  return p;
}

GlCompositor::MaskUniforms GlCompositor::GetMaskUniforms(const GLuint program) {
  MaskUniforms u;
  u.pos_u = glGetUniformLocation(program, "u_pos_u");
  u.pos_v = glGetUniformLocation(program, "u_pos_v");
  u.rrect = glGetUniformLocation(program, "u_rrect");
  u.radii_top = glGetUniformLocation(program, "u_radii_top");
  u.radii_bottom = glGetUniformLocation(program, "u_radii_bottom");
  u.st_clamp = glGetUniformLocation(program, "u_st_clamp");
  u.clip_map = glGetUniformLocation(program, "u_clip_map");
  u.clip_rect = glGetUniformLocation(program, "u_clip_rect");
  u.clip_radii = glGetUniformLocation(program, "u_clip_radii");
  u.clip_count = glGetUniformLocation(program, "u_clip_count");
  return u;
}

void GlCompositor::SetMaskUniforms(const MaskUniforms& u, const QuadMask& m) {
  const auto& r = m.rect.radii;
  if (u.pos_u >= 0) {
    glUniform3f(u.pos_u, m.pos.a, m.pos.c, m.pos.tx);
  }
  if (u.pos_v >= 0) {
    glUniform3f(u.pos_v, m.pos.b, m.pos.d, m.pos.ty);
  }
  if (u.rrect >= 0) {
    const RectF& b = m.rect.rect;
    glUniform4f(u.rrect, static_cast<float>(b.x), static_cast<float>(b.y),
                static_cast<float>(b.x + b.w), static_cast<float>(b.y + b.h));
  }
  if (u.radii_top >= 0) {
    glUniform4f(u.radii_top, r[0][0], r[0][1], r[1][0], r[1][1]);
  }
  if (u.radii_bottom >= 0) {
    glUniform4f(u.radii_bottom, r[3][0], r[3][1], r[2][0], r[2][1]);
  }
  if (u.st_clamp >= 0) {
    glUniform4f(u.st_clamp, m.st_clamp[0], m.st_clamp[1], m.st_clamp[2],
                m.st_clamp[3]);
  }
  if (u.clip_count >= 0) {
    const auto n = static_cast<GLsizei>(m.shapes.count);
    glUniform1i(u.clip_count, n);
    if (n > 0) {
      std::array<GLfloat, 12 * ClipShapes::kMax> map{};
      std::array<GLfloat, 4 * ClipShapes::kMax> rect{};
      std::array<GLfloat, 8 * ClipShapes::kMax> radii{};
      for (size_t k = 0; k < m.shapes.count; ++k) {
        const ClipShape& c = m.shapes.items[k];
        const ViewMap& t = c.to_shape;
        // A pixel's span in the shape's space, padded, rides in the first
        // row's w; past the float range it turns the quick test off.
        const double unit = std::min(1.5 * ClipShapeUnit(c), 1e30);
        const std::array<double, 12> rows = {t.a,  t.c, t.tx, unit, t.b, t.d,
                                             t.ty, 0,   t.g,  t.h,  t.i, 0};
        for (size_t i = 0; i < rows.size(); ++i) {
          map[12 * k + i] = static_cast<GLfloat>(rows[i]);
        }
        const RectF& b = c.shape.rect;
        rect[4 * k] = static_cast<GLfloat>(b.x);
        rect[4 * k + 1] = static_cast<GLfloat>(b.y);
        rect[4 * k + 2] = static_cast<GLfloat>(b.x + b.w);
        rect[4 * k + 3] = static_cast<GLfloat>(b.y + b.h);
        const auto& r = c.shape.radii;
        const std::array<float, 8> rr = {r[0][0], r[0][1], r[1][0], r[1][1],
                                         r[3][0], r[3][1], r[2][0], r[2][1]};
        for (size_t i = 0; i < rr.size(); ++i) {
          radii[8 * k + i] = rr[i];
        }
      }
      glUniform4fv(u.clip_map, 3 * n, map.data());
      glUniform4fv(u.clip_rect, n, rect.data());
      glUniform4fv(u.clip_radii, 2 * n, radii.data());
    }
  }
}

namespace {

// A sampler object bound to a unit overrides the sampling state of the texture
// on it. Flutter's renderer shares this context and can leave one on unit 0,
// and one with a mipmap minification filter makes a texture without mipmaps
// (any imported dma-buf) incomplete, which samples as opaque black. Sampler
// objects are ES3, so there is nothing to do on an ES2 context.
// Not restored: with Skia in compositor mode the embedder calls
// resetContext(kAll_GrBackendState) before rendering each frame
// (SubmitFlutterView in embedder_external_view_embedder.cc), which also drops
// Skia's cached sampler bindings.
void UnbindSampler0(const GlCaps* caps) {
  if (caps != nullptr && caps->bind_sampler != nullptr) {
    caps->bind_sampler(0, 0);
  }
}

}  // namespace

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
  UnbindSampler0(caps_);
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

UvAffine GlCompositor::UvForFlip(const bool flip_y) {
  UvAffine uv;  // identity: a top-first texture maps straight through
  if (!flip_y) {
    uv.d = -1;  // bottom-first: t = 0 (the top) reads the last row
    uv.ty = 1;
  }
  return uv;
}

void GlCompositor::SetLayerUniforms(const GLint uni_uv_u,
                                    const GLint uni_uv_v,
                                    const GLint uni_opaque,
                                    const GLint uni_alpha,
                                    const UvAffine& uv,
                                    const bool opaque,
                                    const float alpha) {
  if (uni_uv_u >= 0) {
    glUniform3f(uni_uv_u, uv.a, uv.c, uv.tx);
  }
  if (uni_uv_v >= 0) {
    glUniform3f(uni_uv_v, uv.b, uv.d, uv.ty);
  }
  if (uni_opaque >= 0) {
    glUniform1f(uni_opaque, opaque ? 1.0f : 0.0f);
  }
  if (uni_alpha >= 0) {
    glUniform1f(uni_alpha, alpha);
  }
}

void GlCompositor::CompositeViaQuadProgram(const QuadProgram& p,
                                           GLenum target,
                                           GLuint tex,
                                           GLint dst_x,
                                           GLint dst_y,
                                           GLsizei dst_w,
                                           GLsizei dst_h,
                                           bool blend,
                                           const UvAffine& uv,
                                           bool opaque,
                                           float alpha,
                                           const QuadMask* mask) {
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

  glUseProgram(p.id);
  glActiveTexture(GL_TEXTURE0);
  UnbindSampler0(caps_);
  // Draw in a vertex array object of our own, and put back the one that was
  // bound. Flutter's renderer shares this context, and the object it leaves
  // bound can carry an instancing divisor on an attribute these programs
  // read per vertex: on a Pi 5 it gave every vertex the first one's w.
  // Binding our own leaves its state alone; changing its divisors would put
  // the renderer's cache of it out of step.
  GLint prev_vao = 0;
  const bool own_vao = caps_ != nullptr && caps_->bind_vertex_array != nullptr;
  if (own_vao) {
    constexpr GLenum kVertexArrayBinding = 0x85B5;  // GL_VERTEX_ARRAY_BINDING
    glGetIntegerv(kVertexArrayBinding, &prev_vao);
    if (quad_vao_ == 0) {
      caps_->gen_vertex_arrays(1, &quad_vao_);
    }
    caps_->bind_vertex_array(quad_vao_);
  }
  if (p.uni_tex >= 0) {
    glUniform1i(p.uni_tex, 0);
  }
  SetLayerUniforms(p.uni_uv_u, p.uni_uv_v, p.uni_opaque, p.uni_alpha, uv,
                   opaque, alpha);
  if (mask != nullptr) {
    SetMaskUniforms(p.mask, *mask);
  }
  if (blend) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  } else {
    glDisable(GL_BLEND);
  }
  // A quad in pieces is one draw of two triangles a piece, each vertex
  // (x, y, u, v, m, w, edge[4]) with m 1 on a corner box, w its homogeneous w
  // and edge its distance to each smoothed edge. A whole quad is the shared
  // strip, and a masked program reads m and w as 1 over all of it, and its
  // edges as hard.
  const bool given = mask != nullptr && !mask->verts.empty();
  const bool parts = given || (mask != nullptr && !mask->parts.empty());
  GLsizei count = 4;
  GLenum mode = GL_TRIANGLE_STRIP;
  GLsizei stride = 4 * sizeof(GLfloat);
  if (parts) {
    if (parts_vbo_ == 0) {
      glGenBuffers(1, &parts_vbo_);
    }
    constexpr size_t kPerVertex = 10;
    std::vector<GLfloat>& verts = scratch_verts_;
    verts.resize(kPerVertex *
                 (given ? mask->verts.size() : 6 * mask->parts.size()));
    size_t n = 0;
    for (size_t i = 0; given && i < mask->verts.size(); ++i) {
      const QuadMask::Vertex& v = mask->verts[i];
      verts[n++] = v.x;
      verts[n++] = v.y;
      verts[n++] = v.s;
      verts[n++] = 1.0f - v.t;
      verts[n++] = v.m;
      verts[n++] = v.w;
      for (const float e : v.edge) {
        verts[n++] = e;
      }
    }
    for (size_t i = 0; !given && i < mask->parts.size(); ++i) {
      const QuadMask::Part& q = mask->parts[i];
      const GLfloat m = q.corner ? 1.0f : 0.0f;
      // (s, t) runs over the viewport with t down; a_uv's v and NDC y run up.
      const auto vertex = [&](const float s, const float t) {
        verts[n++] = 2.0f * s - 1.0f;
        verts[n++] = 1.0f - 2.0f * t;
        verts[n++] = s;
        verts[n++] = 1.0f - t;
        verts[n++] = m;
        verts[n++] = 1.0f;
        for (int e = 0; e < 4; ++e) {
          verts[n++] = kNoEdge;
        }
      };
      vertex(q.s0, q.t0);
      vertex(q.s1, q.t0);
      vertex(q.s0, q.t1);
      vertex(q.s0, q.t1);
      vertex(q.s1, q.t0);
      vertex(q.s1, q.t1);
    }
    glBindBuffer(GL_ARRAY_BUFFER, parts_vbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(n * sizeof(GLfloat)),
                 verts.data(), GL_STREAM_DRAW);
    count = static_cast<GLsizei>(n / kPerVertex);
    mode = GL_TRIANGLES;
    stride = kPerVertex * sizeof(GLfloat);
  } else {
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  }
  if (p.attr_pos >= 0) {
    glEnableVertexAttribArray(static_cast<GLuint>(p.attr_pos));
    glVertexAttribPointer(static_cast<GLuint>(p.attr_pos), 2, GL_FLOAT,
                          GL_FALSE, stride, nullptr);
  }
  if (p.attr_uv >= 0) {
    glEnableVertexAttribArray(static_cast<GLuint>(p.attr_uv));
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    glVertexAttribPointer(static_cast<GLuint>(p.attr_uv), 2, GL_FLOAT, GL_FALSE,
                          stride,
                          reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
  }
  if (p.attr_mask >= 0) {
    if (parts) {
      glEnableVertexAttribArray(static_cast<GLuint>(p.attr_mask));
      // NOLINTNEXTLINE(performance-no-int-to-ptr)
      glVertexAttribPointer(static_cast<GLuint>(p.attr_mask), 1, GL_FLOAT,
                            GL_FALSE, stride,
                            reinterpret_cast<const void*>(4 * sizeof(GLfloat)));
    } else {
      glVertexAttrib1f(static_cast<GLuint>(p.attr_mask), 1.0f);
    }
  }
  if (p.attr_w >= 0) {
    if (parts) {
      glEnableVertexAttribArray(static_cast<GLuint>(p.attr_w));
      // NOLINTNEXTLINE(performance-no-int-to-ptr)
      glVertexAttribPointer(static_cast<GLuint>(p.attr_w), 1, GL_FLOAT,
                            GL_FALSE, stride,
                            reinterpret_cast<const void*>(5 * sizeof(GLfloat)));
    } else {
      glVertexAttrib1f(static_cast<GLuint>(p.attr_w), 1.0f);
    }
  }
  if (p.attr_edge >= 0) {
    if (parts) {
      glEnableVertexAttribArray(static_cast<GLuint>(p.attr_edge));
      // NOLINTNEXTLINE(performance-no-int-to-ptr)
      glVertexAttribPointer(static_cast<GLuint>(p.attr_edge), 4, GL_FLOAT,
                            GL_FALSE, stride,
                            reinterpret_cast<const void*>(6 * sizeof(GLfloat)));
    } else {
      glVertexAttrib4f(static_cast<GLuint>(p.attr_edge), kNoEdge, kNoEdge,
                       kNoEdge, kNoEdge);
    }
  }
  glViewport(dst_x, dst_y, dst_w, dst_h);
  glBindTexture(target, tex);
  glDrawArrays(mode, 0, count);
  glBindTexture(target, 0);
  if (p.attr_pos >= 0) {
    glDisableVertexAttribArray(static_cast<GLuint>(p.attr_pos));
  }
  if (p.attr_uv >= 0) {
    glDisableVertexAttribArray(static_cast<GLuint>(p.attr_uv));
  }
  if (p.attr_mask >= 0 && parts) {
    glDisableVertexAttribArray(static_cast<GLuint>(p.attr_mask));
  }
  if (p.attr_w >= 0 && parts) {
    glDisableVertexAttribArray(static_cast<GLuint>(p.attr_w));
  }
  if (p.attr_edge >= 0 && parts) {
    glDisableVertexAttribArray(static_cast<GLuint>(p.attr_edge));
  }
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  if (own_vao) {
    caps_->bind_vertex_array(static_cast<GLuint>(prev_vao));
  }
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
                                    const UvAffine& uv,
                                    bool opaque,
                                    bool external,
                                    float alpha,
                                    const QuadMask* mask) {
  if (!EnsureQuad()) {
    return;
  }
  if (external && external_.id == 0) {
    // Blank rather than the luma plane in red. Warn once: the driver lacks
    // GL_OES_EGL_image_external and a YUV frame has now actually reached here.
    static bool warned = false;
    if (!warned) {
      warned = true;
      ihs::log::warn(
          "GlCompositor: a YUV platform-view frame needs "
          "GL_OES_EGL_image_external, which this context lacks; it will not be "
          "composited");
    }
    return;
  }
  // A rounded clip needs the masked build; without one it is drawn square.
  if (mask != nullptr && (external ? external_mask_ : mask_).id == 0) {
    static bool warned_no_mask = false;
    if (!warned_no_mask) {
      warned_no_mask = true;
      ihs::log::warn(
          "GlCompositor: the rounded-clip program could not be built; "
          "platform views with rounded corners are drawn square");
    }
    mask = nullptr;
  }
  // The external and masked paths are rare (one platform-view layer), so they
  // do not join the frame-batched fast path: each sets its own program and
  // state and leaves the batched baseline to be re-emitted for the next 2D
  // layer.
  if (external) {
    CompositeViaQuadProgram(mask != nullptr ? external_mask_ : external_,
                            GL_TEXTURE_EXTERNAL_OES, src_color_tex, dst_x,
                            dst_y, dst_w, dst_h, blend, uv, opaque, alpha,
                            mask);
    return;
  }
  if (mask != nullptr) {
    CompositeViaQuadProgram(mask_, GL_TEXTURE_2D, src_color_tex, dst_x, dst_y,
                            dst_w, dst_h, blend, uv, opaque, alpha, mask);
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
    SetLayerUniforms(uni_uv_u_, uni_uv_v_, uni_opaque_, uni_alpha_, uv, opaque,
                     alpha);
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
  UnbindSampler0(caps_);
  glBindTexture(GL_TEXTURE_2D, src_color_tex);
  if (uni_tex_ >= 0) {
    glUniform1i(uni_tex_, 0);
  }
  SetLayerUniforms(uni_uv_u_, uni_uv_v_, uni_opaque_, uni_alpha_, uv, opaque,
                   alpha);

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
  if (caps_ && CanBlit(*caps_, external, blend, src_fbo, dst_fbo)) {
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
  CompositeViaQuad(src_color_tex, dst_x, dst_y, dst_w, dst_h, blend,
                   UvForFlip(flip_y), /*opaque=*/false, external,
                   /*alpha=*/1.0f, /*mask=*/nullptr);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GlCompositor::CompositeLayerToFbo(GLuint dst_fbo,
                                       GLuint tex,
                                       bool external,
                                       const UvAffine& uv,
                                       GLint dst_x,
                                       GLint dst_y,
                                       GLsizei dst_w,
                                       GLsizei dst_h,
                                       bool blend,
                                       bool opaque,
                                       float alpha,
                                       const QuadMask* mask) {
  glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
  CompositeViaQuad(tex, dst_x, dst_y, dst_w, dst_h, blend, uv, opaque, external,
                   alpha, mask);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

bool GlCompositor::CompositeLayerThrough(
    const GLuint dst_fbo,
    const ICompositorSurface::GlLayerTexture& t,
    const RectI& view,
    const GLint fb_height,
    const bool target_top_first,
    const bool under,
    const DisplayRotation* rotation,
    const RectI* clip,
    const float alpha,
    const RoundedRect* rounded,
    const ViewMap& map,
    const ClipShapes* shapes) {
  const auto& g = t.geometry;
  QuadPlacement q;
  if (!PlaceLayerThrough(
          g.src, g.dst, g.transform, g.opaque, static_cast<uint32_t>(t.width),
          static_cast<uint32_t>(t.height), view.w, view.h, map, &q, clip)) {
    return false;
  }
  if (!t.top_first) {
    q.uv = FlipRows(q.uv);
  }
  // The texture coordinates and the mask's position both run over the quad's
  // own (s, t), which no turn of the display changes: only where the quad's
  // corners land does. The viewport is the quad's bounds, so the quad is
  // clipped to them, and to the clip with them.
  QuadMask& mask = scratch_mask_;
  mask.Reset();
  if (rounded != nullptr) {
    mask.rect = *rounded;
  }
  if (shapes != nullptr) {
    mask.shapes = *shapes;
  }
  // The point times w, over w at the corner at (0, 0): the shader scales it
  // by that corner's w over the pixel's, which leaves the point.
  mask.pos.a = static_cast<float>(q.edge_s[0] / q.w0);
  mask.pos.b = static_cast<float>(q.edge_s[1] / q.w0);
  mask.pos.c = static_cast<float>(q.edge_t[0] / q.w0);
  mask.pos.d = static_cast<float>(q.edge_t[1] / q.w0);
  mask.pos.tx = static_cast<float>(q.origin[0] / q.w0);
  mask.pos.ty = static_cast<float>(q.origin[1] / q.w0);
  RectI vp = q.bounds;
  if (rotation != nullptr) {
    vp = rotation->ToPanel(vp);
  }
  const float m = rounded != nullptr || shapes != nullptr ? 1.0f : 0.0f;
  // One vertex at (s, tt) of the quad, with the mask worked out there or not.
  const auto emit = [&](const double s, const double tt, const float vm) {
    const auto p = q.At(s, tt);
    double px = p[0];
    double py = p[1];
    if (rotation != nullptr) {
      rotation->ToPanel(p[0], p[1], &px, &py);
    }
    // Panel pixels, top-left origin, into the viewport's NDC. On a target
    // whose GL y = 0 is its top row, NDC y runs down the panel; otherwise up.
    QuadMask::Vertex& v = mask.verts.emplace_back();
    v.x = static_cast<float>(2.0 * (px - vp.x) / vp.w - 1.0);
    v.y = static_cast<float>(target_top_first
                                 ? 2.0 * (py - vp.y) / vp.h - 1.0
                                 : 2.0 * (vp.y + vp.h - py) / vp.h - 1.0);
    v.s = static_cast<float>(s);
    v.t = static_cast<float>(tt);
    v.m = vm;
    // Relative to the corner at (0, 0), as the mask's position is.
    v.w = static_cast<float>(q.W(s, tt) / q.w0);
    // In output pixels, which a turn of the display does not change.
    const auto e = EdgeDistances(q, p[0], p[1]);
    for (size_t i = 0; i < e.size(); ++i) {
      v.edge[i] = q.grow[i] > 0 ? static_cast<float>(e[i]) : kNoEdge;
    }
  };
  // The quad as drawn, grown past its smoothed edges.
  const auto corners = q.GrownCorners();
  // Clips drawn as shapes cost every pixel they are worked out at, so the
  // quad's bounds are cut into pieces (ClipPieces): those inside every clip
  // and the quad are drawn without them, those near an edge with, cut to the
  // quad as drawn, and those clearly outside not at all. Still one draw,
  // since each draw of an imported buffer can cost a copy of it (see the
  // pieces below).
  std::optional<ViewMap> to_st;
  if (shapes != nullptr) {
    // The quad's map, (s, t, 1) to the output point times w, undone.
    ViewMap quad_map;
    quad_map.a = q.edge_s[0];
    quad_map.c = q.edge_t[0];
    quad_map.tx = q.origin[0];
    quad_map.b = q.edge_s[1];
    quad_map.d = q.edge_t[1];
    quad_map.ty = q.origin[1];
    quad_map.g = q.ws;
    quad_map.h = q.wt;
    quad_map.i = q.w0;
    to_st = InverseMap(quad_map);
  }
  if (to_st) {
    const std::vector<ClipPiece>& pieces =
        piece_cache_.Get(*shapes, &q, q.bounds);
    const std::array<std::array<double, 2>, 4> grown = {
        q.At(corners[0][0], corners[0][1]), q.At(corners[1][0], corners[1][1]),
        q.At(corners[3][0], corners[3][1]), q.At(corners[2][0], corners[2][1])};
    for (const ClipPiece& piece : pieces) {
      const RectI& r = piece.rect;
      ConvexPolygon poly = {
          {static_cast<double>(r.x), static_cast<double>(r.y)},
          {static_cast<double>(r.x + r.w), static_cast<double>(r.y)},
          {static_cast<double>(r.x + r.w), static_cast<double>(r.y + r.h)},
          {static_cast<double>(r.x), static_cast<double>(r.y + r.h)}};
      if (piece.mask) {
        poly = CutPolygonToQuad(poly, grown, 0.0);
      }
      const float vm = piece.mask ? 1.0f : 0.0f;
      for (size_t i = 1; i + 1 < poly.size(); ++i) {
        for (const auto& p : {poly[0], poly[i], poly[i + 1]}) {
          const auto st = to_st->Map(p[0], p[1]);
          emit(st[0], st[1], vm);
        }
      }
    }
  }
  if (mask.verts.empty()) {
    for (const size_t k : {0U, 1U, 2U, 2U, 1U, 3U}) {
      emit(corners[k][0], corners[k][1], m);
    }
  }
  const auto held = SampleClamp(q, static_cast<uint32_t>(t.width),
                                static_cast<uint32_t>(t.height));
  for (size_t i = 0; i < held.size(); ++i) {
    mask.st_clamp[i] = static_cast<float>(held[i]);
  }
  const GLint gl_y = target_top_first ? vp.y : fb_height - vp.y - vp.h;
  // Outside the quad nothing is drawn, so inside it the rule is a rect's: an
  // opaque layer overwrites what is under it unless the view is translucent,
  // its corners are cut or its edges smoothed.
  const bool blend =
      under && (!q.opaque || alpha < 1.0f || rounded != nullptr ||
                shapes != nullptr || q.Smoothed());
  CompositeLayerToFbo(dst_fbo, t.name, t.external, q.uv, vp.x, gl_y, vp.w, vp.h,
                      blend, q.opaque, alpha, &mask);
  return true;
}

size_t GlCompositor::CompositeSurfaceLayers(
    GLuint dst_fbo,
    const ICompositorSurface& surface,
    const RectI& view,
    GLint fb_height,
    bool target_top_first,
    bool blend_first,
    const std::function<void(const ICompositorSurface::GlLayerTexture&)>&
        on_drawn,
    const ICompositorSurface::GlLayerTexture* layer0,
    const DisplayRotation* rotation,
    const RectI* clip,
    float alpha,
    const RoundedRect* rounded,
    const ViewMap* map,
    const ClipShapes* shapes) {
  // Clips drawn as shapes take the place of the rounded clip. More than the
  // programs take leaves the rounded clip as it was, with some corners
  // square.
  if (shapes != nullptr && shapes->count == 0) {
    shapes = nullptr;
  }
  // The programs are built on first use, which sets how many they take.
  if (shapes != nullptr &&
      (!EnsureQuad() || mask_.id == 0 || shapes->count > clip_slots_)) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      ihs::log::warn(
          "GlCompositor: a platform view has {} clips that are not rects on "
          "screen, more than the {} this context's programs take; they are "
          "drawn as their bounding boxes",
          shapes->count, clip_slots_);
    }
    shapes = nullptr;
  }
  if (shapes != nullptr) {
    rounded = nullptr;
  }
  // A transformed view is drawn through the self-contained programs. Without
  // them it falls back to the rect, as it was drawn before.
  if (map != nullptr && (!EnsureQuad() || mask_.id == 0)) {
    static bool warned = false;
    if (!warned) {
      warned = true;
      ihs::log::warn(
          "GlCompositor: the program for transformed platform views could not "
          "be built; rotated and skewed views are drawn upright");
    }
    map = nullptr;
  }
  size_t drawn = 0;
  const size_t count = surface.GetLayerCount();
  for (size_t i = 0; i < count; ++i) {
    const ICompositorSurface::GlLayerTexture t =
        i == 0 && layer0 != nullptr ? *layer0 : surface.GetLayerGlTexture(i);
    if (t.name == 0) {
      continue;
    }
    if (map != nullptr && (!t.external || external_mask_.id != 0)) {
      if (CompositeLayerThrough(dst_fbo, t, view, fb_height, target_top_first,
                                blend_first || drawn > 0, rotation, clip, alpha,
                                rounded, *map, shapes)) {
        ++drawn;
        if (on_drawn) {
          on_drawn(t);
        }
      }
      continue;
    }
    const auto& g = t.geometry;
    LayerPlacement p;
    if (!PlaceLayer(g.src, g.dst, g.transform, g.opaque,
                    static_cast<uint32_t>(t.width),
                    static_cast<uint32_t>(t.height), view, &p, clip)) {
      continue;
    }
    // UvAffine counts rows from the buffer's first row in memory. A texture
    // stored bottom row first (a GL render target) has that row at v = 1.
    if (!t.top_first) {
      p.uv = FlipRows(p.uv);
    }
    // The rounded clip is in render pixels, like p.dst before it turns: the
    // mask's position affine takes (s, t) there, and from here on takes every
    // turn the texture coordinates do.
    //
    // Only the boxes around the corners need the mask, so the quad is drawn
    // in pieces and the mask worked out only in those boxes. The pieces stay
    // one draw: a driver that cannot sample a linear buffer (v3d) samples a
    // tiled copy, and refreshes the copy for every draw that reads an imported
    // buffer, since it cannot know whether the producer wrote it. As separate
    // draws, a 1280x1440 view on a Pi 5 took seven copies a frame and fell to
    // 30 frames a second.
    QuadMask& mask = scratch_mask_;
    mask.Reset();
    std::vector<ClipPiece>& pieces = scratch_pieces_;
    pieces.clear();
    if (rounded != nullptr) {
      std::array<RoundedPiece, 7> corners;
      const size_t count = SplitRoundedCorners(p.dst, *rounded, corners);
      for (size_t k = 0; k < count; ++k) {
        pieces.push_back({corners[k].rect, corners[k].corner});
      }
      mask.rect = *rounded;
    }
    // Clips drawn as shapes are worked out only near their edges.
    if (shapes != nullptr) {
      mask.shapes = *shapes;
      pieces = piece_cache_.Get(*shapes, nullptr, p.dst);
    }
    if (rounded != nullptr || shapes != nullptr) {
      mask.pos.a = static_cast<float>(p.dst.w);
      mask.pos.b = 0.0f;
      mask.pos.c = 0.0f;
      mask.pos.d = static_cast<float>(p.dst.h);
      mask.pos.tx = static_cast<float>(p.dst.x);
      mask.pos.ty = static_cast<float>(p.dst.y);
    }
    // Clipped to the view in render space above; only the draw turns.
    if (rotation != nullptr) {
      p.dst = rotation->ToPanel(p.dst);
      p.uv = rotation->ToPanel(p.uv);
      mask.pos = rotation->ToPanel(mask.pos);
    }
    GLint gl_y = fb_height - p.dst.y - p.dst.h;
    if (target_top_first) {
      // GL's y = 0 is the target's top row, so the rect goes in unconverted
      // and the quad's t axis (top to bottom in window terms) turns over.
      gl_y = p.dst.y;
      for (UvAffine* a : {&p.uv, &mask.pos}) {
        a->tx += a->c;
        a->c = -a->c;
        a->ty += a->d;
        a->d = -a->d;
      }
    }
    // Each piece into the quad's (s, t) as drawn: the mask's position affine
    // takes (s, t) to render pixels, so its inverse brings a piece back. Turns
    // and row flips keep a rect a rect.
    if (rounded != nullptr || shapes != nullptr) {
      const UvAffine& m = mask.pos;
      const double det =
          static_cast<double>(m.a) * m.d - static_cast<double>(m.b) * m.c;
      if (std::abs(det) > 1e-12 && !pieces.empty()) {
        const auto st = [&](const double x, const double y) {
          const double dx = x - m.tx;
          const double dy = y - m.ty;
          return std::array<double, 2>{(m.d * dx - m.c * dy) / det,
                                       (m.a * dy - m.b * dx) / det};
        };
        for (const ClipPiece& piece : pieces) {
          const RectI& r = piece.rect;
          const auto a = st(r.x, r.y);
          const auto b = st(r.x + r.w, r.y + r.h);
          QuadMask::Part& q = mask.parts.emplace_back();
          q.s0 = static_cast<float>(std::clamp(std::min(a[0], b[0]), 0.0, 1.0));
          q.s1 = static_cast<float>(std::clamp(std::max(a[0], b[0]), 0.0, 1.0));
          q.t0 = static_cast<float>(std::clamp(std::min(a[1], b[1]), 0.0, 1.0));
          q.t1 = static_cast<float>(std::clamp(std::max(a[1], b[1]), 0.0, 1.0));
          q.corner = piece.mask;
        }
      }
    }
    // A layer that covers its rect overwrites what is under it, unless the
    // view is translucent or its corners are cut: then it blends like any
    // other. The first layer with nothing under it still overwrites; its
    // premultiplied output is what blending onto a cleared target would give.
    const bool blend =
        (blend_first || drawn > 0) &&
        (!p.opaque || alpha < 1.0f || rounded != nullptr || shapes != nullptr);
    CompositeLayerToFbo(
        dst_fbo, t.name, t.external, p.uv, p.dst.x, gl_y, p.dst.w, p.dst.h,
        blend, p.opaque, alpha,
        rounded != nullptr || shapes != nullptr ? &mask : nullptr);
    ++drawn;
    if (on_drawn) {
      on_drawn(t);
    }
  }
  return drawn;
}
