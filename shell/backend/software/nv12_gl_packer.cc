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

#include "backend/software/nv12_gl_packer.h"

#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstring>

#include "logging/logging.h"

namespace {

constexpr uint32_t kDrmFormatR8 = 0x20203852U;  // 'R','8',' ',' '

int AllocDmabuf(size_t size) {
  static const char* kHeaps[] = {"/dev/dma_heap/linux,cma",
                                 "/dev/dma_heap/default_cma_region",
                                 "/dev/dma_heap/system"};
  for (const char* path : kHeaps) {
    const int heap = ::open(path, O_RDWR | O_CLOEXEC);
    if (heap < 0) {
      continue;
    }
    dma_heap_allocation_data a{};
    a.len = size;
    a.fd_flags = O_RDWR | O_CLOEXEC;
    const int rc = ::ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &a);
    ::close(heap);
    if (rc == 0) {
      return static_cast<int>(a.fd);
    }
  }
  return -1;
}

GLuint Compile(GLenum stage, const char* src) {
  GLuint s = glCreateShader(stage);
  glShaderSource(s, 1, &src, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (ok == 0) {
    char log[512];
    glGetShaderInfoLog(s, sizeof log, nullptr, log);
    ihs::log::error("[Nv12GlPacker] shader compile failed: {}", log);
    glDeleteShader(s);
    return 0;
  }
  return s;
}

const char* kVert =
    "#version 300 es\n"
    "in vec2 pos;\n"
    "void main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n";

// Pack RGBA -> NV12 into a stride x (H*3/2) R8 target. gl_FragCoord.xy is the
// destination byte (column, row). Limited-range BT.601. The source is a GL FBO
// texture, whose origin is bottom-left, so sample with the row flipped to keep
// NV12 (top-down) upright.
const char* kFrag =
    "#version 300 es\n"
    "precision highp float;\n"
    "uniform sampler2D src;\n"
    "uniform float uW, uH;\n"
    "out vec4 frag;\n"
    "float luma(vec3 c){ return (0.257*c.r+0.504*c.g+0.098*c.b)+16.0/255.0; }\n"
    "float cb(vec3 c){ return (-0.148*c.r-0.291*c.g+0.439*c.b)+128.0/255.0; }\n"
    "float cr(vec3 c){ return (0.439*c.r-0.368*c.g-0.071*c.b)+128.0/255.0; }\n"
    "void main(){\n"
    "  float x = floor(gl_FragCoord.x);\n"
    "  float y = floor(gl_FragCoord.y);\n"
    "  float outv;\n"
    "  if (y < uH) {\n"
    "    vec2 s = vec2((x+0.5)/uW, 1.0-(y+0.5)/uH);\n"
    "    outv = luma(texture(src, s).rgb);\n"
    "  } else {\n"
    "    float cyf = y - uH;\n"
    "    float cxf = floor(x*0.5);\n"
    "    vec2 s = vec2((cxf*2.0+1.0)/uW, 1.0-(cyf*2.0+1.0)/uH);\n"
    "    vec3 c = texture(src, s).rgb;\n"
    "    outv = (mod(x,2.0) < 1.0) ? cb(c) : cr(c);\n"
    "  }\n"
    "  frag = vec4(outv, 0.0, 0.0, 1.0);\n"
    "}\n";

}  // namespace

Nv12GlPacker::~Nv12GlPacker() {
  Teardown();
}

bool Nv12GlPacker::ImportSlot(EGLDisplay dpy, Slot& slot) {
  const size_t size = static_cast<size_t>(stride_) * height_ * 3 / 2;
  slot.fd = AllocDmabuf(size);
  if (slot.fd < 0) {
    ihs::log::error("[Nv12GlPacker] dma-heap alloc of {} bytes failed", size);
    return false;
  }
  const EGLint attrs[] = {EGL_WIDTH,
                          static_cast<EGLint>(stride_),
                          EGL_HEIGHT,
                          static_cast<EGLint>(height_ * 3 / 2),
                          EGL_LINUX_DRM_FOURCC_EXT,
                          static_cast<EGLint>(kDrmFormatR8),
                          EGL_DMA_BUF_PLANE0_FD_EXT,
                          slot.fd,
                          EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                          0,
                          EGL_DMA_BUF_PLANE0_PITCH_EXT,
                          static_cast<EGLint>(stride_),
                          EGL_NONE};
  auto create = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
      eglGetProcAddress("eglCreateImageKHR"));
  auto target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
      eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (create == nullptr || target == nullptr) {
    ihs::log::error("[Nv12GlPacker] missing EGLImage entry points");
    return false;
  }
  slot.image =
      create(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
  if (slot.image == EGL_NO_IMAGE_KHR) {
    ihs::log::error("[Nv12GlPacker] eglCreateImageKHR failed: {:#x}",
                    eglGetError());
    return false;
  }
  glGenTextures(1, &slot.tex);
  glBindTexture(GL_TEXTURE_2D, slot.tex);
  target(GL_TEXTURE_2D, static_cast<GLeglImageOES>(slot.image));
  glGenFramebuffers(1, &slot.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, slot.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         slot.tex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    ihs::log::error("[Nv12GlPacker] FBO on the imported dma-buf is incomplete");
    return false;
  }
  return true;
}

bool Nv12GlPacker::Init(EGLDisplay dpy,
                        uint32_t width,
                        uint32_t height,
                        INv12Consumer* consumer) {
  dpy_ = dpy;
  consumer_ = consumer;
  width_ = width & ~1u;
  height_ = height & ~1u;
  stride_ = width_;
  if (width_ == 0 || height_ == 0 || consumer_ == nullptr) {
    return false;
  }

  GLuint vs = Compile(GL_VERTEX_SHADER, kVert);
  GLuint fs = Compile(GL_FRAGMENT_SHADER, kFrag);
  if (vs == 0 || fs == 0) {
    return false;
  }
  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glBindAttribLocation(program_, 0, "pos");
  glLinkProgram(program_);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint linked = 0;
  glGetProgramiv(program_, GL_LINK_STATUS, &linked);
  if (linked == 0) {
    ihs::log::error("[Nv12GlPacker] program link failed");
    return false;
  }
  u_src_ = glGetUniformLocation(program_, "src");
  u_w_ = glGetUniformLocation(program_, "uW");
  u_h_ = glGetUniformLocation(program_, "uH");

  // Own VAO so the pack draw is isolated from whatever vertex-array state the
  // Flutter engine leaves bound. Sharing the engine's VAO means its still-
  // enabled attributes (pointing at its own buffers) get pulled into our draw
  // and the triangle is silently discarded. A dedicated VAO with only attrib 0
  // enabled fixes that.
  const GLfloat tri[] = {-1.f, -1.f, 3.f, -1.f, -1.f, 3.f};
  glGenVertexArrays(1, &vao_);
  glBindVertexArray(vao_);
  glGenBuffers(1, &vbo_);
  glBindBuffer(GL_ARRAY_BUFFER, vbo_);
  glBufferData(GL_ARRAY_BUFFER, sizeof tri, tri, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
  glBindVertexArray(0);

  for (uint32_t i = 0; i < kRingSize; ++i) {
    if (!ImportSlot(dpy_, ring_[i])) {
      Teardown();
      return false;
    }
    ring_[i].in_flight = false;
    slot_ref_[i] = {this, i};
  }
  if (!consumer_->Configure(width_, height_, stride_)) {
    ihs::log::error("[Nv12GlPacker] consumer rejected {}x{}", width_, height_);
    Teardown();
    return false;
  }
  ready_ = true;
  ihs::log::info("[Nv12GlPacker] ready ({}x{}, stride {}, ring {})", width_,
                 height_, stride_, kRingSize);
  return true;
}

void Nv12GlPacker::ReleaseSlotTrampoline(void* ctx) {
  auto* ref = static_cast<SlotRef*>(ctx);
  ref->packer->ReleaseSlot(ref->index);
}

void Nv12GlPacker::ReleaseSlot(uint32_t index) {
  std::lock_guard<std::mutex> lock(ring_mu_);
  ring_[index].in_flight = false;
}

void Nv12GlPacker::PackAndSubmit(GLuint rgba_tex,
                                 uint64_t timestamp_us,
                                 bool force_keyframe) {
  if (!ready_) {
    return;
  }
  uint32_t idx = kRingSize;
  {
    std::lock_guard<std::mutex> lock(ring_mu_);
    for (uint32_t i = 0; i < kRingSize; ++i) {
      if (!ring_[i].in_flight) {
        idx = i;
        break;
      }
    }
    if (idx == kRingSize) {
      return;  // ring full: drop
    }
    ring_[idx].in_flight = true;
  }

  Slot& slot = ring_[idx];
  // The Flutter engine leaves its own GL state (blend, scissor, depth/stencil,
  // colour mask) set after rendering the frame. Reset what the pack pass needs
  // so a stray blend or masked channel can't corrupt the packed NV12.
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_CULL_FACE);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBindFramebuffer(GL_FRAMEBUFFER, slot.fbo);
  glViewport(0, 0, static_cast<GLsizei>(stride_),
             static_cast<GLsizei>(height_ * 3 / 2));
  glUseProgram(program_);
  glUniform1i(u_src_, 0);
  glUniform1f(u_w_, static_cast<float>(width_));
  glUniform1f(u_h_, static_cast<float>(height_));
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, rgba_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glFinish();  // the dma-buf must be readable by the encoder before we submit

  const bool took =
      consumer_->OnFrame(slot.fd, nullptr, timestamp_us, force_keyframe,
                         &ReleaseSlotTrampoline, &slot_ref_[idx]);
  if (!took) {
    ReleaseSlot(idx);
  }
}

void Nv12GlPacker::Teardown() {
  ready_ = false;
  for (auto& slot : ring_) {
    if (slot.fbo != 0) {
      glDeleteFramebuffers(1, &slot.fbo);
    }
    if (slot.tex != 0) {
      glDeleteTextures(1, &slot.tex);
    }
    if (slot.image != nullptr && dpy_ != nullptr) {
      auto destroy = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
          eglGetProcAddress("eglDestroyImageKHR"));
      if (destroy != nullptr) {
        destroy(dpy_, slot.image);
      }
    }
    if (slot.fd >= 0) {
      ::close(slot.fd);
    }
    slot = {};
  }
  if (program_ != 0) {
    glDeleteProgram(program_);
    program_ = 0;
  }
  if (vbo_ != 0) {
    glDeleteBuffers(1, &vbo_);
    vbo_ = 0;
  }
  if (vao_ != 0) {
    glDeleteVertexArrays(1, &vao_);
    vao_ = 0;
  }
}
