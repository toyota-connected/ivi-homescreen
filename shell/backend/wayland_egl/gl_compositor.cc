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

// a_uv is (0, 0) at the viewport's bottom-left. (s, t) is the same point with
// t running top to bottom, which is the space UvAffine is written in, so a
// source crop and a buffer transform are one affine away.
//
// Every shader here is built twice, with IHS_MASK 0 and 1 (ShaderSource). The
// masked build cuts a quad's corners to a rounded clip; the other has none of
// that code, so a quad without a clip pays nothing for it. With the mask,
// v_pos is the point in the clip's space (QuadMask::pos), through an affine
// that takes the same turns as the texture coordinates.
constexpr char kVertSrc[] =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "uniform vec3 u_uv_u;\n"
    "uniform vec3 u_uv_v;\n"
    "varying vec2 v_uv;\n"
    "#if IHS_MASK\n"
    "uniform vec3 u_pos_u;\n"
    "uniform vec3 u_pos_v;\n"
    "varying vec2 v_pos;\n"
    "#endif\n"
    "void main() {\n"
    "  vec3 st = vec3(a_uv.x, 1.0 - a_uv.y, 1.0);\n"
    "  v_uv = vec2(dot(u_uv_u, st), dot(u_uv_v, st));\n"
    "#if IHS_MASK\n"
    "  v_pos = vec2(dot(u_pos_u, st), dot(u_pos_v, st));\n"
    "#endif\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

constexpr char kFragPrecision[] = "precision mediump float;\n";

// The share of a pixel inside a rounded rect, for a pixel the quad's crop has
// already put inside its bounding box: only the corners are left to cut, with
// about a pixel of edge. A corner with a zero radius is square. Positions are
// in pixels, up to a few thousand, which mediump cannot place to a pixel, so
// this runs in highp where the GPU has it. Without the mask it is 1.
constexpr char kMaskSrc[] =
    "#if IHS_MASK\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "#define MASKP highp\n"
    "#else\n"
    "#define MASKP mediump\n"
    "#endif\n"
    "varying MASKP vec2 v_pos;\n"
    "uniform MASKP vec4 u_rrect;\n"         // left, top, right, bottom
    "uniform MASKP vec4 u_radii_top;\n"     // upper left x y, upper right x y
    "uniform MASKP vec4 u_radii_bottom;\n"  // lower left x y, lower right x y
    "float Coverage() {\n"
    "  MASKP vec2 p = v_pos;\n"
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
    "#else\n"
    "float Coverage() { return 1.0; }\n"
    "#endif\n";

// u_opaque forces alpha to 1: an XRGB buffer's alpha is undefined, and
// premultiplied color is unchanged by it. u_alpha is the view's opacity; the
// color is premultiplied, so it and the mask's coverage scale all four
// channels.
constexpr char kFragBody[] =
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_opaque;\n"
    "uniform float u_alpha;\n"
    "void main() {\n"
    "  vec4 c = texture2D(u_tex, v_uv);\n"
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
    "varying vec2 v_uv;\n"
    "uniform samplerExternalOES u_tex;\n"
    "uniform float u_opaque;\n"
    "uniform float u_alpha;\n"
    "void main() {\n"
    "  vec4 c = texture2D(u_tex, v_uv);\n"
    "  gl_FragColor =\n"
    "      vec4(c.rgb, mix(c.a, 1.0, u_opaque)) * (u_alpha * Coverage());\n"
    "}\n";

// The vertex and fragment source of the quad program: external or 2D, with or
// without the rounded-clip mask.
std::string VertSource(const bool mask) {
  return std::string("#define IHS_MASK ") + (mask ? "1" : "0") + "\n" +
         kVertSrc;
}
std::string FragSource(const bool external, const bool mask) {
  return std::string(external ? kFragExtension : "") + "#define IHS_MASK " +
         (mask ? "1" : "0") + "\n" + kFragPrecision + kMaskSrc +
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
                    FragSource(/*external=*/false, /*mask=*/false).c_str());
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
  mask_ = BuildQuadProgram(/*external=*/false, /*mask=*/true);
  // Optional: a driver without the extension keeps the packed-format path, and
  // a planar frame is then simply not composited rather than mis-sampled.
  const auto* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  if (!ihs::gl::ExtensionSupported(exts, "GL_OES_EGL_image_external")) {
    // Left unbuilt, not an error: a context that never presents a YUV
    // platform-view frame does not need it. The one warning, if a frame does
    // arrive, is emitted where the composite is refused.
    return;
  }
  external_ = BuildQuadProgram(/*external=*/true, /*mask=*/false);
  if (external_.id != 0) {
    external_mask_ = BuildQuadProgram(/*external=*/true, /*mask=*/true);
  }
}

GlCompositor::QuadProgram GlCompositor::BuildQuadProgram(const bool external,
                                                         const bool mask) {
  QuadProgram p;
  const GLuint vs = CompileShader(GL_VERTEX_SHADER, VertSource(mask).c_str());
  const GLuint fs =
      CompileShader(GL_FRAGMENT_SHADER, FragSource(external, mask).c_str());
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
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  if (p.attr_pos >= 0) {
    glEnableVertexAttribArray(static_cast<GLuint>(p.attr_pos));
    glVertexAttribPointer(static_cast<GLuint>(p.attr_pos), 2, GL_FLOAT,
                          GL_FALSE, 4 * sizeof(GLfloat), nullptr);
  }
  if (p.attr_uv >= 0) {
    glEnableVertexAttribArray(static_cast<GLuint>(p.attr_uv));
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    glVertexAttribPointer(static_cast<GLuint>(p.attr_uv), 2, GL_FLOAT, GL_FALSE,
                          4 * sizeof(GLfloat),
                          reinterpret_cast<const void*>(2 * sizeof(GLfloat)));
  }
  glViewport(dst_x, dst_y, dst_w, dst_h);
  glBindTexture(target, tex);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindTexture(target, 0);
  if (p.attr_pos >= 0) {
    glDisableVertexAttribArray(static_cast<GLuint>(p.attr_pos));
  }
  if (p.attr_uv >= 0) {
    glDisableVertexAttribArray(static_cast<GLuint>(p.attr_uv));
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
    const RoundedRect* rounded) {
  size_t drawn = 0;
  const size_t count = surface.GetLayerCount();
  for (size_t i = 0; i < count; ++i) {
    const ICompositorSurface::GlLayerTexture t =
        i == 0 && layer0 != nullptr ? *layer0 : surface.GetLayerGlTexture(i);
    if (t.name == 0) {
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
    QuadMask mask;
    if (rounded != nullptr) {
      mask.rect = *rounded;
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
    // A layer that covers its rect overwrites what is under it, unless the
    // view is translucent or its corners are cut: then it blends like any
    // other. The first layer with nothing under it still overwrites; its
    // premultiplied output is what blending onto a cleared target would give.
    const bool blend = (blend_first || drawn > 0) &&
                       (!p.opaque || alpha < 1.0f || rounded != nullptr);
    CompositeLayerToFbo(dst_fbo, t.name, t.external, p.uv, p.dst.x, gl_y,
                        p.dst.w, p.dst.h, blend, p.opaque, alpha,
                        rounded != nullptr ? &mask : nullptr);
    ++drawn;
    if (on_drawn) {
      on_drawn(t);
    }
  }
  return drawn;
}
