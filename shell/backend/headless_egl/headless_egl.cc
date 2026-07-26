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

#include "backend/headless_egl/headless_egl.h"

#include <EGL/eglext.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <ctime>

#include "backend/gl_process_resolver.h"
#include "engine.h"
#include "logging/logging.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"

namespace {

uint64_t MonotonicUs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000 +
         static_cast<uint64_t>(ts.tv_nsec) / 1000;
}

HeadlessEglBackend* BackendOf(void* user_data) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  return reinterpret_cast<HeadlessEglBackend*>(
      state->view_controller->engine->GetBackend());
}

}  // namespace

HeadlessEglBackend::HeadlessEglBackend(uint32_t initial_width,
                                       uint32_t initial_height,
                                       std::unique_ptr<INv12Consumer> consumer)
    : width_(initial_width & ~1u),
      height_(initial_height & ~1u),
      consumer_(std::move(consumer)) {
  const char* node = std::getenv("IVI_ENC_RENDER_NODE");
  if (!InitEgl(node != nullptr ? node : "/dev/dri/renderD128")) {
    ihs::log::error("[HeadlessEgl] EGL init failed; backend inert");
  }
}

HeadlessEglBackend::~HeadlessEglBackend() {
  // Quiesce the consumer before tearing down the GL resources its held frames
  // point into.
  if (dpy_ != EGL_NO_DISPLAY) {
    eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx_);
  }
  consumer_.reset();
  Teardown();
}

bool HeadlessEglBackend::InitEgl(const char* render_node) {
  render_fd_ = ::open(render_node, O_RDWR | O_CLOEXEC);
  if (render_fd_ < 0) {
    ihs::log::error("[HeadlessEgl] open {} failed", render_node);
    return false;
  }
  gbm_ = gbm_create_device(render_fd_);
  if (gbm_ == nullptr) {
    ihs::log::error("[HeadlessEgl] gbm_create_device failed");
    return false;
  }
  auto get_platform = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
      eglGetProcAddress("eglGetPlatformDisplayEXT"));
  dpy_ = get_platform != nullptr
             ? get_platform(EGL_PLATFORM_GBM_KHR, gbm_, nullptr)
             : eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(gbm_));
  EGLint major = 0;
  EGLint minor = 0;
  if (dpy_ == EGL_NO_DISPLAY || eglInitialize(dpy_, &major, &minor) == 0) {
    ihs::log::error("[HeadlessEgl] eglInitialize failed");
    return false;
  }
  const char* exts = eglQueryString(dpy_, EGL_EXTENSIONS);
  if (exts == nullptr ||
      std::strstr(exts, "EGL_EXT_image_dma_buf_import") == nullptr) {
    ihs::log::error("[HeadlessEgl] no EGL_EXT_image_dma_buf_import");
    return false;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
                              EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                              EGL_RED_SIZE,        8,
                              EGL_GREEN_SIZE,      8,
                              EGL_BLUE_SIZE,       8,
                              EGL_ALPHA_SIZE,      8,
                              EGL_NONE};
  EGLConfig cfg = nullptr;
  EGLint ncfg = 0;
  if (eglChooseConfig(dpy_, cfg_attrs, &cfg, 1, &ncfg) == 0 || ncfg == 0) {
    ihs::log::error("[HeadlessEgl] eglChooseConfig failed");
    return false;
  }
  const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  ctx_ = eglCreateContext(dpy_, cfg, EGL_NO_CONTEXT, ctx_attrs);
  // The engine's resource thread needs its own context sharing objects with
  // the render context.
  resource_ctx_ = eglCreateContext(dpy_, cfg, ctx_, ctx_attrs);
  if (ctx_ == EGL_NO_CONTEXT || resource_ctx_ == EGL_NO_CONTEXT) {
    ihs::log::error("[HeadlessEgl] eglCreateContext failed");
    return false;
  }
  ihs::log::info("[HeadlessEgl] EGL {}.{} ready on {}", major, minor,
                 render_node);
  return true;
}

bool HeadlessEglBackend::InitRenderTarget() {
  if (target_ready_) {
    return true;
  }
  if (width_ == 0 || height_ == 0 || consumer_ == nullptr) {
    return false;
  }
  glGenTextures(1, &render_tex_);
  glBindTexture(GL_TEXTURE_2D, render_tex_);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width_),
               static_cast<GLsizei>(height_), 0, GL_RGBA, GL_UNSIGNED_BYTE,
               nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glGenFramebuffers(1, &render_fbo_);
  glBindFramebuffer(GL_FRAMEBUFFER, render_fbo_);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         render_tex_, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    ihs::log::error("[HeadlessEgl] render FBO incomplete");
    return false;
  }
  if (!packer_.Init(dpy_, width_, height_, consumer_.get())) {
    ihs::log::error("[HeadlessEgl] packer init failed");
    return false;
  }
  target_ready_ = true;
  ihs::log::info("[HeadlessEgl] render target ready {}x{}", width_, height_);
  return true;
}

bool HeadlessEglBackend::MakeCurrent() {
  if (dpy_ == EGL_NO_DISPLAY ||
      eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx_) == 0) {
    return false;
  }
  return InitRenderTarget();
}

bool HeadlessEglBackend::ClearCurrent() {
  return dpy_ != EGL_NO_DISPLAY &&
         eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        EGL_NO_CONTEXT) != 0;
}

bool HeadlessEglBackend::MakeResourceCurrent() {
  return dpy_ != EGL_NO_DISPLAY &&
         eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        resource_ctx_) != 0;
}

bool HeadlessEglBackend::Present() {
  if (!target_ready_) {
    return false;
  }
  packer_.PackAndSubmit(render_tex_, MonotonicUs(),
                        /*force_keyframe=*/frame_index_ == 0);
  ++frame_index_;
  return true;
}

FlutterRendererConfig HeadlessEglBackend::GetRenderConfig() {
  FlutterRendererConfig config{};
  config.type = kOpenGL;
  config.open_gl.struct_size = sizeof(FlutterOpenGLRendererConfig);
  config.open_gl.make_current = [](void* user_data) -> bool {
    return BackendOf(user_data)->MakeCurrent();
  };
  config.open_gl.clear_current = [](void* user_data) -> bool {
    return BackendOf(user_data)->ClearCurrent();
  };
  config.open_gl.make_resource_current = [](void* user_data) -> bool {
    return BackendOf(user_data)->MakeResourceCurrent();
  };
  config.open_gl.fbo_callback = [](void* user_data) -> uint32_t {
    return BackendOf(user_data)->Fbo();
  };
  config.open_gl.present = [](void* user_data) -> bool {
    return BackendOf(user_data)->Present();
  };
  config.open_gl.fbo_reset_after_present = false;
  config.open_gl.gl_proc_resolver = [](void* /*user_data*/,
                                       const char* name) -> void* {
    return GlProcessResolver::GetInstance().process_resolver(name);
  };
  return config;
}

FlutterCompositor HeadlessEglBackend::GetCompositorConfig() {
  // Single-FBO rendering: no platform-view layer compositing (the encode path
  // wants one composited frame). A software/GL compositor could be added later.
  FlutterCompositor compositor{};
  compositor.struct_size = sizeof(FlutterCompositor);
  return compositor;
}

void HeadlessEglBackend::Resize(size_t /*index*/,
                                Engine* /*flutter_engine*/,
                                int32_t width,
                                int32_t height) {
  const uint32_t w = static_cast<uint32_t>(width) & ~1u;
  const uint32_t h = static_cast<uint32_t>(height) & ~1u;
  if (w == width_ && h == height_) {
    return;
  }
  // Geometry changes are not yet supported mid-run (the packer/consumer would
  // need re-init). Log and keep the initial size.
  ihs::log::warn("[HeadlessEgl] resize to {}x{} ignored (fixed at {}x{})", w, h,
                 width_, height_);
}

void HeadlessEglBackend::CreateSurface(size_t /*index*/,
                                       wl_surface* /*surface*/,
                                       int32_t /*width*/,
                                       int32_t /*height*/) {
  // No display surface: rendering targets our own FBO, set up lazily on the
  // first MakeCurrent from the raster thread.
}

bool HeadlessEglBackend::TextureMakeCurrent() {
  return MakeResourceCurrent();
}

bool HeadlessEglBackend::TextureClearCurrent() {
  return ClearCurrent();
}

void HeadlessEglBackend::Teardown() {
  if (dpy_ == EGL_NO_DISPLAY) {
    return;
  }
  if (render_fbo_ != 0) {
    glDeleteFramebuffers(1, &render_fbo_);
  }
  if (render_tex_ != 0) {
    glDeleteTextures(1, &render_tex_);
  }
  eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (resource_ctx_ != EGL_NO_CONTEXT) {
    eglDestroyContext(dpy_, resource_ctx_);
  }
  if (ctx_ != EGL_NO_CONTEXT) {
    eglDestroyContext(dpy_, ctx_);
  }
  eglTerminate(dpy_);
  dpy_ = EGL_NO_DISPLAY;
  if (gbm_ != nullptr) {
    gbm_device_destroy(gbm_);
  }
  if (render_fd_ >= 0) {
    ::close(render_fd_);
  }
}
