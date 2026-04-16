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

#include "backend/drm_kms_egl/drm_compositor.h"

#include <cerrno>
#include <cstring>
#include <utility>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "backend/drm_kms_egl/drm_backend.h"
#include "logging.h"
#include "view/compositor_surface_interface.h"

namespace {

constexpr uint32_t kBackingStoreFormat = GBM_FORMAT_ARGB8888;

}  // namespace

DrmCompositor::DrmCompositor(DrmBackend* backend) : backend_(backend) {}

DrmCompositor::~DrmCompositor() {
  if (texture_blit_fbo_ != 0) {
    glDeleteFramebuffers(1, &texture_blit_fbo_);
    texture_blit_fbo_ = 0;
  }
  gl_compositor_.reset();

  // Destroy all live backing stores (GL + GBM + KMS resources).
  for (auto& [baton, store] : stores_) {
    DestroyGbmStore(*store);
    delete baton;
  }
  stores_.clear();
}

bool DrmCompositor::InitEglExtensions() {
  eglCreateImageKHR_ = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
      eglGetProcAddress("eglCreateImageKHR"));
  eglDestroyImageKHR_ = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
      eglGetProcAddress("eglDestroyImageKHR"));
  glEGLImageTargetTexture2DOES_ =
      reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
          eglGetProcAddress("glEGLImageTargetTexture2DOES"));

  if (!eglCreateImageKHR_ || !eglDestroyImageKHR_ ||
      !glEGLImageTargetTexture2DOES_) {
    spdlog::error(
        "[DrmCompositor] required EGL/GL extensions not available "
        "(EGL_KHR_image_base + GL_OES_EGL_image)");
    return false;
  }
  return true;
}

void DrmCompositor::EnsureGlCapsProbed() {
  if (gl_caps_probed_) {
    return;
  }
  gl_caps_.Probe();
  gl_compositor_ = std::make_unique<GlCompositor>(&gl_caps_);
  InitEglExtensions();
  gl_caps_probed_ = true;
}

uint32_t DrmCompositor::ImportBoAsFb(gbm_bo* bo) {
  const uint32_t w = gbm_bo_get_width(bo);
  const uint32_t h = gbm_bo_get_height(bo);
  const uint32_t format = gbm_bo_get_format(bo);
  uint32_t handles[4] = {gbm_bo_get_handle(bo).u32, 0, 0, 0};
  uint32_t pitches[4] = {gbm_bo_get_stride(bo), 0, 0, 0};
  uint32_t offsets[4] = {0, 0, 0, 0};
  uint32_t fb_id = 0;
  if (drmModeAddFB2(backend_->drm_fd(), w, h, format, handles, pitches,
                    offsets, &fb_id, 0) != 0) {
    spdlog::error("[DrmCompositor] drmModeAddFB2: {}", std::strerror(errno));
    return 0;
  }
  return fb_id;
}

void DrmCompositor::DestroyGbmStore(GbmBackingStore& s) {
  if (s.fbo != 0) {
    glDeleteFramebuffers(1, &s.fbo);
  }
  if (s.color_tex != 0) {
    glDeleteTextures(1, &s.color_tex);
  }
  if (s.depth_stencil_rb != 0) {
    glDeleteRenderbuffers(1, &s.depth_stencil_rb);
  }
  if (s.egl_image != EGL_NO_IMAGE_KHR && eglDestroyImageKHR_) {
    eglDestroyImageKHR_(backend_->egl_display(), s.egl_image);
  }
  if (s.drm_fb_id != 0) {
    drmModeRmFB(backend_->drm_fd(), s.drm_fb_id);
  }
  if (s.bo) {
    gbm_bo_destroy(s.bo);
  }
}

bool DrmCompositor::CreateBackingStore(const FlutterBackingStoreConfig* config,
                                       FlutterBackingStore* out) {
  EnsureGlCapsProbed();

  const auto w = static_cast<uint32_t>(config->size.width);
  const auto h = static_cast<uint32_t>(config->size.height);

  auto store = std::make_unique<GbmBackingStore>();
  store->width = w;
  store->height = h;

  // 1. GBM buffer object — renderable + scanout-capable.
  store->bo = gbm_bo_create(backend_->gbm(), w, h, kBackingStoreFormat,
                            GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
  if (!store->bo) {
    spdlog::error("[DrmCompositor] gbm_bo_create {}x{}: {}", w, h,
                  std::strerror(errno));
    return false;
  }

  // 2. EGL image wrapping the BO (Mesa native-pixmap path).
  store->egl_image = eglCreateImageKHR_(
      backend_->egl_display(), EGL_NO_CONTEXT, EGL_NATIVE_PIXMAP_KHR,
      reinterpret_cast<EGLClientBuffer>(store->bo), nullptr);
  if (store->egl_image == EGL_NO_IMAGE_KHR) {
    spdlog::error("[DrmCompositor] eglCreateImageKHR: 0x{:x}", eglGetError());
    DestroyGbmStore(*store);
    return false;
  }

  // 3. GL texture backed by the EGL image — sampleable for the GL
  //    compositor path AND usable as an FBO color attachment.
  glGenTextures(1, &store->color_tex);
  glBindTexture(GL_TEXTURE_2D, store->color_tex);
  glEGLImageTargetTexture2DOES_(GL_TEXTURE_2D,
                                static_cast<GLeglImageOES>(store->egl_image));
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  // 4. FBO with the image-backed texture as color + a depth/stencil RB.
  glGenFramebuffers(1, &store->fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, store->fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         store->color_tex, 0);

  glGenRenderbuffers(1, &store->depth_stencil_rb);
  glBindRenderbuffer(GL_RENDERBUFFER, store->depth_stencil_rb);
  if (gl_caps_.has_packed_depth_stencil) {
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES,
                          static_cast<GLsizei>(w), static_cast<GLsizei>(h));
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, store->depth_stencil_rb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, store->depth_stencil_rb);
  } else {
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                          static_cast<GLsizei>(w), static_cast<GLsizei>(h));
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                              GL_RENDERBUFFER, store->depth_stencil_rb);
  }

  const GLenum fb_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (fb_status != GL_FRAMEBUFFER_COMPLETE) {
    spdlog::error("[DrmCompositor] FBO incomplete: 0x{:x}", fb_status);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    DestroyGbmStore(*store);
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // 5. KMS framebuffer for direct scan-out on an overlay plane.
  store->drm_fb_id = ImportBoAsFb(store->bo);
  if (store->drm_fb_id == 0) {
    DestroyGbmStore(*store);
    return false;
  }

  // Hand the store to the engine.
  auto* baton = new StoreBaton{this, store.get()};

  out->struct_size = sizeof(FlutterBackingStore);
  out->type = kFlutterBackingStoreTypeOpenGL;
  out->user_data = baton;
  out->open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
  out->open_gl.framebuffer.target =
      gl_caps_.has_rgb8_rgba8 ? GL_RGBA8_OES : GL_RGBA;
  out->open_gl.framebuffer.name = store->fbo;
  out->open_gl.framebuffer.user_data = baton;
  out->open_gl.framebuffer.destruction_callback = [](void*) {};

  stores_[baton] = std::move(store);
  return true;
}

bool DrmCompositor::CollectBackingStore(const FlutterBackingStore* store) {
  auto* baton = static_cast<StoreBaton*>(store->user_data);
  if (!baton) {
    return false;
  }
  auto it = stores_.find(baton);
  if (it != stores_.end()) {
    DestroyGbmStore(*it->second);
    stores_.erase(it);
  }
  delete baton;
  return true;
}

void DrmCompositor::CompositeBackingStore(const GbmBackingStore* store,
                                          GLint dst_x,
                                          GLint dst_y,
                                          GLsizei dst_w,
                                          GLsizei dst_h,
                                          bool blend) {
  gl_compositor_->CompositeToDefault(
      store->fbo, store->color_tex, static_cast<GLsizei>(store->width),
      static_cast<GLsizei>(store->height), dst_x, dst_y, dst_w, dst_h, blend);
}

void DrmCompositor::CompositePlatformView(const FlutterLayer* layer,
                                          bool blend) {
  std::shared_ptr<ICompositorSurface> surface_sp;
  {
    std::lock_guard<std::mutex> lock(surfaces_mu_);
    const auto it = surfaces_.find(layer->platform_view->identifier);
    if (it != surfaces_.end()) {
      surface_sp = it->second;
    }
  }
  if (!surface_sp) {
    spdlog::warn("[DrmCompositor] platform view {} not registered",
                 layer->platform_view->identifier);
    return;
  }

  auto& surface = *surface_sp;
  surface.OnPresent(layer);

  const auto tex = surface.GetGlTextureName();
  if (tex == 0) {
    return;
  }

  const auto sw = surface.GetGlTextureWidth();
  const auto sh = surface.GetGlTextureHeight();
  const auto dx = static_cast<GLint>(layer->offset.x);
  const auto dw = static_cast<GLsizei>(layer->size.width);
  const auto dh = static_cast<GLsizei>(layer->size.height);
  const auto dy = static_cast<GLint>(backend_->height()) -
                  static_cast<GLint>(layer->offset.y) - dh;
  constexpr bool kFlipY = true;

  if (!blend && gl_caps_.has_blit_framebuffer) {
    if (!texture_blit_fbo_) {
      glGenFramebuffers(1, &texture_blit_fbo_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, texture_blit_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex, 0);
    gl_compositor_->CompositeToDefault(texture_blit_fbo_, tex, sw, sh, dx, dy,
                                       dw, dh, blend, kFlipY);
  } else {
    gl_compositor_->CompositeToDefault(0, tex, sw, sh, dx, dy, dw, dh, blend,
                                       kFlipY);
  }
}

bool DrmCompositor::PresentLayers(const FlutterLayer** layers,
                                  size_t layer_count) {
  EnsureGlCapsProbed();

  // GL composition into the default framebuffer. The plane-allocator path
  // (step 3) will replace this with direct scan-out for layers that fit on
  // hardware planes, falling back here for the rest.
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  bool composited_any = false;
  for (size_t i = 0; i < layer_count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (!layer) {
      continue;
    }
    const bool blend = composited_any;
    if (layer->type == kFlutterLayerContentTypeBackingStore &&
        layer->backing_store) {
      auto* baton =
          static_cast<StoreBaton*>(layer->backing_store->user_data);
      if (baton && baton->store) {
        const auto dx = static_cast<GLint>(layer->offset.x);
        const auto dy = static_cast<GLint>(layer->offset.y);
        const auto dw = static_cast<GLsizei>(layer->size.width);
        const auto dh = static_cast<GLsizei>(layer->size.height);
        CompositeBackingStore(baton->store, dx, dy, dw, dh, blend);
        composited_any = true;
      }
    } else if (layer->type == kFlutterLayerContentTypePlatformView &&
               layer->platform_view) {
      CompositePlatformView(layer, blend);
      composited_any = true;
    }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return backend_->Present();
}

void DrmCompositor::RegisterSurface(
    FlutterPlatformViewIdentifier id,
    std::shared_ptr<ICompositorSurface> surface) {
  std::lock_guard<std::mutex> lock(surfaces_mu_);
  surfaces_[id] = std::move(surface);
}

void DrmCompositor::UnregisterSurface(FlutterPlatformViewIdentifier id) {
  std::lock_guard<std::mutex> lock(surfaces_mu_);
  surfaces_.erase(id);
}

void DrmCompositor::ResizeSurface(FlutterPlatformViewIdentifier id,
                                   int32_t width,
                                   int32_t height) {
  std::shared_ptr<ICompositorSurface> surface_sp;
  {
    std::lock_guard<std::mutex> lock(surfaces_mu_);
    const auto it = surfaces_.find(id);
    if (it != surfaces_.end()) {
      surface_sp = it->second;
    }
  }
  if (surface_sp) {
    surface_sp->OnResize(width, height);
  }
}
