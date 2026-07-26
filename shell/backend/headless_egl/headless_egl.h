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
#include <GLES3/gl3.h>

#include <cstdint>
#include <memory>

#include "backend/backend.h"
#include "backend/software/nv12_consumer.h"
#include "backend/software/nv12_gl_packer.h"

struct gbm_device;
class Engine;

// GPU headless encode backend: the Flutter engine renders each frame on the GPU
// (kOpenGL) into an FBO this backend owns -- no display, no Wayland, no scanout
// -- and on present the frame is packed RGBA->NV12 on the GPU straight into a
// dma-buf and handed to an INv12Consumer (a file encoder or a WebRTC send). It
// is the zero-copy counterpart of the software backend's CPU EncoderSink: the
// GPU both rasterizes and colour-converts, so no CPU touches the pixels.
//
// Surfaceless GBM/EGL on the render node (like pi_gl_encode). Consumer selected
// by IVI_ENC_SINK (default encoder:file:out.h264), same spec grammar as the
// software backend's IVI_SW_SINK.
class HeadlessEglBackend final : public Backend {
 public:
  HeadlessEglBackend(uint32_t initial_width,
                     uint32_t initial_height,
                     std::unique_ptr<INv12Consumer> consumer);
  ~HeadlessEglBackend() override;

  void Resize(size_t index,
              Engine* flutter_engine,
              int32_t width,
              int32_t height) override;
  void CreateSurface(size_t index,
                     wl_surface* surface,
                     int32_t width,
                     int32_t height) override;

  bool TextureMakeCurrent() override;
  bool TextureClearCurrent() override;

  FlutterRendererConfig GetRenderConfig() override;
  FlutterCompositor GetCompositorConfig() override;

  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }

  // Called from the engine's OpenGL renderer config trampolines (raster thread).
  bool MakeCurrent();
  bool ClearCurrent();
  bool MakeResourceCurrent();
  uint32_t Fbo() const { return render_fbo_; }
  bool Present();  // engine finished the frame -> pack + submit

 private:
  bool InitEgl(const char* render_node);
  bool InitRenderTarget();  // the RGBA FBO + the packer; GL context must be current
  void Teardown();

  uint32_t width_{0};
  uint32_t height_{0};
  uint64_t frame_index_{0};

  int render_fd_{-1};
  gbm_device* gbm_{nullptr};
  EGLDisplay dpy_{EGL_NO_DISPLAY};
  EGLContext ctx_{EGL_NO_CONTEXT};
  EGLContext resource_ctx_{EGL_NO_CONTEXT};

  GLuint render_tex_{0};  // RGBA colour attachment the engine draws into
  GLuint render_fbo_{0};
  bool target_ready_{false};

  std::unique_ptr<INv12Consumer> consumer_;
  Nv12GlPacker packer_;
};
