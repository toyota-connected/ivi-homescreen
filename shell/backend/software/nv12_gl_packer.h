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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <cstdint>
#include <memory>
#include <mutex>

#include "backend/software/nv12_consumer.h"

// GPU RGBA -> NV12 packer for the headless-EGL encode path: given the Flutter
// frame as a GL RGBA texture (the render FBO's colour attachment) and a current
// GLES3 context, it converts to NV12 straight into a dma-buf and hands that
// buffer to an INv12Consumer -- no CPU touches the pixels. Each NV12 buffer is
// one contiguous CMA dma-heap allocation imported as a single tall R8 EGL image
// (Y in the top rows, interleaved CbCr below), packed by one fragment shader,
// so there is no two-plane / channel-order ambiguity. A ring of them lets an
// asynchronous consumer (a WebRTC send) hold a frame while the next is packed.
//
// The port of the validated pi_gl_encode tool. Must be constructed and called
// with the packer's GL context current (the backend's raster thread).
class Nv12GlPacker {
 public:
  Nv12GlPacker() = default;
  ~Nv12GlPacker();

  Nv12GlPacker(const Nv12GlPacker&) = delete;
  Nv12GlPacker& operator=(const Nv12GlPacker&) = delete;

  // Allocate the ring for `width`x`height` and configure the consumer. `dpy` is
  // the packer's EGL display (for dma-buf import). Returns false on failure.
  bool Init(EGLDisplay dpy,
            uint32_t width,
            uint32_t height,
            INv12Consumer* consumer);

  // Convert `rgba_tex` (a GL_TEXTURE_2D RGBA of the configured size) to NV12 in
  // a free ring slot and submit it to the consumer. `timestamp_us` and
  // `force_keyframe` pass through. Drops the frame if no slot is free. Must run
  // with the packer's GL context current.
  void PackAndSubmit(GLuint rgba_tex,
                     uint64_t timestamp_us,
                     bool force_keyframe);

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

 private:
  static constexpr uint32_t kRingSize = 4;

  struct Slot {
    int fd{-1};  // NV12 dma-buf (single, packed)
    EGLImageKHR image{nullptr};
    GLuint tex{0};  // R8 view of the whole buffer
    GLuint fbo{0};
    bool in_flight{false};
  };

  bool ImportSlot(EGLDisplay dpy, Slot& slot);
  void ReleaseSlot(uint32_t index);
  static void ReleaseSlotTrampoline(void* ctx);
  void Teardown();

  struct SlotRef {
    Nv12GlPacker* packer;
    uint32_t index;
  };

  EGLDisplay dpy_{nullptr};
  INv12Consumer* consumer_{nullptr};
  uint32_t width_{0};
  uint32_t height_{0};
  uint32_t stride_{0};
  Slot ring_[kRingSize];
  SlotRef slot_ref_[kRingSize];
  std::mutex ring_mu_;  // guards Slot::in_flight (release fires off-thread)

  GLuint program_{0};
  GLuint vbo_{0};
  GLuint vao_{0};
  uint64_t last_push_us_{0};  // last submitted frame, for the push-rate cap
  GLint u_src_{-1};
  GLint u_w_{-1};
  GLint u_h_{-1};
  bool ready_{false};
};
