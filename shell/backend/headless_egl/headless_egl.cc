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
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

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
  const bool ctx_current =
      dpy_ != EGL_NO_DISPLAY &&
      eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx_) != 0;
  consumer_.reset();
  // Free the packer's GL/dma-buf resources now, while our context is current --
  // Teardown() below destroys that context, and the packer's own destructor
  // (member, runs after this body) would then glDelete* with none current.
  if (ctx_current) {
    packer_.Shutdown();
  }
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
  // An OPAQUE window-surface config (no alpha), native visual XRGB8888.
  // Opacity matters: an alpha surface lets a translucent app background (e.g. a
  // withOpacity gradient over a transparent Scaffold) composite to
  // premultiplied near-zero and vanish on the destroyed swap buffers; an opaque
  // surface makes the engine composite a full opaque frame, so the background
  // is present.
  const EGLint cfg_attrs[] = {EGL_SURFACE_TYPE,
                              EGL_WINDOW_BIT,
                              EGL_RENDERABLE_TYPE,
                              EGL_OPENGL_ES3_BIT,
                              EGL_RED_SIZE,
                              8,
                              EGL_GREEN_SIZE,
                              8,
                              EGL_BLUE_SIZE,
                              8,
                              EGL_ALPHA_SIZE,
                              0,
                              EGL_NONE};
  EGLint total = 0;
  if (eglChooseConfig(dpy_, cfg_attrs, nullptr, 0, &total) == 0 || total == 0) {
    ihs::log::error("[HeadlessEgl] eglChooseConfig found no configs");
    return false;
  }
  std::vector<EGLConfig> configs(static_cast<size_t>(total));
  eglChooseConfig(dpy_, cfg_attrs, configs.data(), total, &total);
  gbm_format_ = GBM_FORMAT_XRGB8888;
  for (EGLConfig c : configs) {
    EGLint vid = 0;
    EGLint alpha = 0;
    eglGetConfigAttrib(dpy_, c, EGL_NATIVE_VISUAL_ID, &vid);
    eglGetConfigAttrib(dpy_, c, EGL_ALPHA_SIZE, &alpha);
    if (alpha == 0 && static_cast<uint32_t>(vid) == GBM_FORMAT_XRGB8888) {
      config_ = c;
      gbm_format_ = GBM_FORMAT_XRGB8888;
      break;
    }
  }
  if (config_ == nullptr) {
    config_ = configs.front();  // fall back; the surface create may still work
  }

  const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
  ctx_ = eglCreateContext(dpy_, config_, EGL_NO_CONTEXT, ctx_attrs);
  // The engine's resource thread needs its own context sharing objects with
  // the render context.
  resource_ctx_ = eglCreateContext(dpy_, config_, ctx_, ctx_attrs);
  if (ctx_ == EGL_NO_CONTEXT || resource_ctx_ == EGL_NO_CONTEXT) {
    ihs::log::error("[HeadlessEgl] eglCreateContext failed");
    return false;
  }

  // The swap chain: a gbm_surface the driver double-buffers, wrapped in an EGL
  // window surface. Force a LINEAR modifier so the presented buffers are
  // untiled and can be re-imported per frame as a plain GL_TEXTURE_2D for the
  // NV12 pack (see Present); V3D otherwise hands back a tiled buffer that an
  // implicit-modifier import mis-reads as dashed/blocky garbage. Fall back to a
  // plain create if the driver rejects the forced modifier.
  const uint64_t linear_mod = DRM_FORMAT_MOD_LINEAR;
  gbm_surface_ = gbm_surface_create_with_modifiers(gbm_, width_, height_,
                                                   gbm_format_, &linear_mod, 1);
  if (gbm_surface_ == nullptr) {
    gbm_surface_ = gbm_surface_create(gbm_, width_, height_, gbm_format_,
                                      GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
  }
  if (gbm_surface_ == nullptr) {
    ihs::log::error("[HeadlessEgl] gbm_surface_create {}x{} failed", width_,
                    height_);
    return false;
  }
  auto create_platform_surface =
      reinterpret_cast<PFNEGLCREATEPLATFORMWINDOWSURFACEEXTPROC>(
          eglGetProcAddress("eglCreatePlatformWindowSurfaceEXT"));
  egl_surface_ =
      create_platform_surface != nullptr
          ? create_platform_surface(dpy_, config_, gbm_surface_, nullptr)
          : eglCreateWindowSurface(
                dpy_, config_,
                reinterpret_cast<EGLNativeWindowType>(gbm_surface_), nullptr);
  if (egl_surface_ == EGL_NO_SURFACE) {
    ihs::log::error("[HeadlessEgl] eglCreateWindowSurface failed: {:#x}",
                    eglGetError());
    return false;
  }

  ihs::log::info("[HeadlessEgl] EGL {}.{} ready on {} (XRGB8888)", major, minor,
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
  // A single reusable texture the presented (linear) gbm_bo is imported into
  // each frame for the pack.
  glGenTextures(1, &import_tex_);
  if (!packer_.Init(dpy_, width_, height_, consumer_.get())) {
    ihs::log::error("[HeadlessEgl] packer init failed");
    return false;
  }
  target_ready_ = true;
  ihs::log::info("[HeadlessEgl] render target ready {}x{}", width_, height_);
  return true;
}

bool HeadlessEglBackend::MakeCurrent() {
  if (dpy_ == EGL_NO_DISPLAY || egl_surface_ == EGL_NO_SURFACE ||
      eglMakeCurrent(dpy_, egl_surface_, egl_surface_, ctx_) == 0) {
    return false;
  }
  return InitRenderTarget();
}

bool HeadlessEglBackend::ClearCurrent() {
  return dpy_ != EGL_NO_DISPLAY &&
         eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) !=
             0;
}

bool HeadlessEglBackend::MakeResourceCurrent() {
  // The resource context does no windowed rendering, so keep it surfaceless.
  return dpy_ != EGL_NO_DISPLAY &&
         eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, resource_ctx_) !=
             0;
}

bool HeadlessEglBackend::Present(const FlutterPresentInfo* /*info*/) {
  if (!target_ready_) {
    return false;
  }
  // Advance the swap chain: the driver publishes the frame the engine just
  // composited as the new front buffer (the full frame lands here on swap, not
  // in the pre-swap back buffer).
  if (eglSwapBuffers(dpy_, egl_surface_) == 0) {
    ihs::log::error("[HeadlessEgl] eglSwapBuffers failed: {:#x}",
                    eglGetError());
    return false;
  }
  gbm_bo* bo = gbm_surface_lock_front_buffer(gbm_surface_);
  if (bo == nullptr) {
    ihs::log::error("[HeadlessEgl] lock_front_buffer failed");
    return false;
  }

  // Import the presented (linear) buffer as a plain 2D texture and pack it. The
  // buffer is linear (forced at surface creation), so pass its modifier through
  // for an exact import.
  const int fd = gbm_bo_get_fd(bo);
  const EGLint stride = static_cast<EGLint>(gbm_bo_get_stride(bo));
  const uint64_t modifier = gbm_bo_get_modifier(bo);
  const bool has_modifier = modifier != DRM_FORMAT_MOD_INVALID;
  std::vector<EGLint> attrs = {EGL_WIDTH,
                               static_cast<EGLint>(width_),
                               EGL_HEIGHT,
                               static_cast<EGLint>(height_),
                               EGL_LINUX_DRM_FOURCC_EXT,
                               static_cast<EGLint>(gbm_bo_get_format(bo)),
                               EGL_DMA_BUF_PLANE0_FD_EXT,
                               fd,
                               EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                               0,
                               EGL_DMA_BUF_PLANE0_PITCH_EXT,
                               stride};
  if (has_modifier) {
    attrs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT);
    attrs.push_back(static_cast<EGLint>(modifier & 0xffffffff));
    attrs.push_back(EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT);
    attrs.push_back(static_cast<EGLint>(modifier >> 32));
  }
  attrs.push_back(EGL_NONE);
  auto create_image = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
      eglGetProcAddress("eglCreateImageKHR"));
  auto destroy_image = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
      eglGetProcAddress("eglDestroyImageKHR"));
  auto image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
      eglGetProcAddress("glEGLImageTargetTexture2DOES"));
  if (create_image != nullptr && destroy_image != nullptr &&
      image_target != nullptr) {
    EGLImageKHR image = create_image(
        dpy_, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs.data());
    if (image != EGL_NO_IMAGE_KHR) {
      glBindTexture(GL_TEXTURE_2D, import_tex_);
      image_target(GL_TEXTURE_2D, static_cast<GLeglImageOES>(image));
      // The presented gbm_bo is top-left origin, so no vertical flip.
      packer_.PackAndSubmit(import_tex_, MonotonicUs(),
                            /*force_keyframe=*/frame_index_ == 0,
                            /*flip_rows=*/false);
      destroy_image(dpy_, image);
    } else {
      ihs::log::error("[HeadlessEgl] import eglCreateImageKHR failed: {:#x}",
                      eglGetError());
    }
  }
  if (fd >= 0) {
    ::close(fd);
  }

  // Release the buffer we held last frame; keep this one locked until the next
  // swap so the import's read is stable.
  if (locked_bo_ != nullptr) {
    gbm_surface_release_buffer(gbm_surface_, locked_bo_);
  }
  locked_bo_ = bo;
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
  // Render into the window surface's default framebuffer (FBO 0); the driver
  // rotates the gbm buffers on eglSwapBuffers and Present() imports the
  // published front buffer. populate_existing_damage reports the whole surface
  // as stale so the engine composites a complete frame each time (an encoder
  // wants a full frame per buffer, not a partial repaint).
  config.open_gl.fbo_callback = [](void* /*user_data*/) -> uint32_t {
    return 0;
  };
  config.open_gl.present_with_info =
      [](void* user_data, const FlutterPresentInfo* info) -> bool {
    return BackendOf(user_data)->Present(info);
  };
  // Deliberately NOT providing populate_existing_damage: partial repaint
  // against the rotated (destroyed) swap buffers intermittently dropped the
  // static background layer (a whole-run black background on ~1 in 4 starts).
  // Without it the engine composites a complete frame into the buffer every
  // time, which is what an encoder wants anyway.
  config.open_gl.fbo_reset_after_present = false;
  config.open_gl.gl_proc_resolver = [](void* /*user_data*/,
                                       const char* name) -> void* {
    return GlProcessResolver::GetInstance().process_resolver(name);
  };
  return config;
}

FlutterCompositor HeadlessEglBackend::GetCompositorConfig() {
  // Single-surface rendering: no platform-view layer compositing (the encode
  // path wants one composited frame). A GL compositor could be added later.
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
  // Geometry changes are not yet supported mid-run (the swap chain / packer /
  // consumer would need re-init). Log and keep the initial size.
  ihs::log::warn("[HeadlessEgl] resize to {}x{} ignored (fixed at {}x{})", w, h,
                 width_, height_);
}

void HeadlessEglBackend::CreateSurface(size_t /*index*/,
                                       wl_surface* /*surface*/,
                                       int32_t /*width*/,
                                       int32_t /*height*/) {
  // No display surface: rendering targets our own gbm-backed EGL window
  // surface, set up in InitEgl and bound on the first MakeCurrent.
}

bool HeadlessEglBackend::TextureMakeCurrent() {
  return MakeResourceCurrent();
}

bool HeadlessEglBackend::TextureClearCurrent() {
  return ClearCurrent();
}

void HeadlessEglBackend::Teardown() {
  // The GL/EGL objects only exist once we have a display, so tear those down
  // under a dpy_ guard. The gbm device and render-node fd are opened before EGL
  // comes up (see InitEgl), so they must be released even when EGL init failed
  // part-way and dpy_ is still EGL_NO_DISPLAY -- otherwise a partial init leaks
  // the fd. Handles are reset as they go so this is safe to call more than
  // once.
  if (dpy_ != EGL_NO_DISPLAY) {
    // glDelete* needs a current context. If MakeCurrent fails (or was never
    // established) skip the explicit deletes -- eglTerminate below frees them.
    const bool ctx_current =
        ctx_ != EGL_NO_CONTEXT &&
        eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx_) != 0;
    if (ctx_current && import_tex_ != 0) {
      glDeleteTextures(1, &import_tex_);
    }
    import_tex_ = 0;
    eglMakeCurrent(dpy_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_surface_ != EGL_NO_SURFACE) {
      eglDestroySurface(dpy_, egl_surface_);
      egl_surface_ = EGL_NO_SURFACE;
    }
    if (resource_ctx_ != EGL_NO_CONTEXT) {
      eglDestroyContext(dpy_, resource_ctx_);
      resource_ctx_ = EGL_NO_CONTEXT;
    }
    if (ctx_ != EGL_NO_CONTEXT) {
      eglDestroyContext(dpy_, ctx_);
      ctx_ = EGL_NO_CONTEXT;
    }
    eglTerminate(dpy_);
    dpy_ = EGL_NO_DISPLAY;
  }
  // The front buffer must be released back to the surface before it is
  // destroyed.
  if (locked_bo_ != nullptr && gbm_surface_ != nullptr) {
    gbm_surface_release_buffer(gbm_surface_, locked_bo_);
  }
  locked_bo_ = nullptr;
  if (gbm_surface_ != nullptr) {
    gbm_surface_destroy(gbm_surface_);
    gbm_surface_ = nullptr;
  }
  if (gbm_ != nullptr) {
    gbm_device_destroy(gbm_);
    gbm_ = nullptr;
  }
  if (render_fd_ >= 0) {
    ::close(render_fd_);
    render_fd_ = -1;
  }
}
