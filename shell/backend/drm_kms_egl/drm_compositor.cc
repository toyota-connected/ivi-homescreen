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

#include <utility>

#include "backend/drm_kms_egl/drm_backend.h"
#include "logging.h"
#include "view/compositor_surface_interface.h"

namespace {

// Sized color internal format — ES3 / OES_rgb8_rgba8 lets us request an
// explicit RGBA8; pure ES2 falls back to unsized GL_RGBA. Mirrors the
// wayland_egl backend's choice so plugin-shared textures format-match.
GLenum PickColorInternalFormat(const GlCaps& caps) {
  return caps.has_rgb8_rgba8 ? GL_RGBA8_OES : GL_RGBA;
}

}  // namespace

DrmCompositor::DrmCompositor(DrmBackend* backend) : backend_(backend) {}

DrmCompositor::~DrmCompositor() {
  // Destructor must run with the EGL context current — DrmBackend ensures
  // this by holding the compositor as a member and resetting it before
  // tearing EGL down. Deleting the blit FBO and the GlCompositor's own
  // shader/VBO here is safe under that invariant.
  if (texture_blit_fbo_ != 0) {
    glDeleteFramebuffers(1, &texture_blit_fbo_);
    texture_blit_fbo_ = 0;
  }
  gl_compositor_.reset();
  stores_.clear();
}

void DrmCompositor::EnsureGlCapsProbed() {
  if (gl_caps_probed_) {
    return;
  }
  gl_caps_.Probe();
  gl_compositor_ = std::make_unique<GlCompositor>(&gl_caps_);
  gl_caps_probed_ = true;
}

bool DrmCompositor::CreateBackingStore(const FlutterBackingStoreConfig* config,
                                       FlutterBackingStore* out) {
  EnsureGlCapsProbed();

  const auto w = static_cast<int32_t>(config->size.width);
  const auto h = static_cast<int32_t>(config->size.height);

  auto store = std::make_unique<EglFboBackingStore>(w, h, &gl_caps_);
  auto* baton = new StoreBaton{this, store.get()};

  out->struct_size = sizeof(FlutterBackingStore);
  out->type = kFlutterBackingStoreTypeOpenGL;
  out->user_data = baton;
  out->open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
  out->open_gl.framebuffer.target = PickColorInternalFormat(gl_caps_);
  out->open_gl.framebuffer.name = store->Framebuffer();
  out->open_gl.framebuffer.user_data = baton;
  // The engine only calls this when it drops the backing-store reference
  // without going through collect_backing_store_callback (rare). Leave the
  // actual resource release to CollectBackingStore, which is the documented
  // matching call.
  out->open_gl.framebuffer.destruction_callback = [](void*) {};

  stores_[baton] = std::move(store);
  return true;
}

bool DrmCompositor::CollectBackingStore(const FlutterBackingStore* store) {
  auto* baton = static_cast<StoreBaton*>(store->user_data);
  if (!baton) {
    return false;
  }
  stores_.erase(baton);
  delete baton;
  return true;
}

void DrmCompositor::CompositeBackingStore(const FlutterBackingStore* store,
                                          GLint dst_x,
                                          GLint dst_y,
                                          GLsizei dst_w,
                                          GLsizei dst_h,
                                          bool blend) {
  auto* baton = static_cast<StoreBaton*>(store->user_data);
  if (!baton || !baton->store) {
    spdlog::error("[DrmCompositor] layer with null backing-store baton");
    return;
  }
  auto* fbo = baton->store;
  gl_compositor_->CompositeToDefault(fbo->Framebuffer(), fbo->ColorTexture(),
                                     fbo->Width(), fbo->Height(), dst_x, dst_y,
                                     dst_w, dst_h, blend);
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
    // Plugin handles its own presentation; nothing to composite here.
    return;
  }

  const auto sw = surface.GetGlTextureWidth();
  const auto sh = surface.GetGlTextureHeight();
  const auto dx = static_cast<GLint>(layer->offset.x);
  const auto dw = static_cast<GLsizei>(layer->size.width);
  const auto dh = static_cast<GLsizei>(layer->size.height);
  // Flutter is top-left origin; the default framebuffer is bottom-left.
  const auto dy = static_cast<GLint>(backend_->height()) -
                  static_cast<GLint>(layer->offset.y) - dh;
  // Plugin-supplied textures are in GL-native (bottom-up) coordinates and
  // must be flipped to match Flutter's top-down layout.
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

  // Clear the window backbuffer. No alpha fall-through to worry about on
  // DRM (no compositor below us), but clearing prevents stale pixels from
  // the swapchain showing through wherever no layer covers them.
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  // Engine emits layers bottom-to-top. The first composited layer lands as
  // an opaque copy; every subsequent layer alpha-blends so transparent
  // pixels preserve what's already underneath.
  bool composited_any = false;
  for (size_t i = 0; i < layer_count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (!layer) {
      continue;
    }
    const bool blend = composited_any;
    if (layer->type == kFlutterLayerContentTypeBackingStore &&
        layer->backing_store) {
      const auto dx = static_cast<GLint>(layer->offset.x);
      const auto dy = static_cast<GLint>(layer->offset.y);
      const auto dw = static_cast<GLsizei>(layer->size.width);
      const auto dh = static_cast<GLsizei>(layer->size.height);
      CompositeBackingStore(layer->backing_store, dx, dy, dw, dh, blend);
      composited_any = true;
    } else if (layer->type == kFlutterLayerContentTypePlatformView &&
               layer->platform_view) {
      CompositePlatformView(layer, blend);
      composited_any = true;
    }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  // Hand off to the backend to swap the EGL buffer and queue the page
  // flip. In compositor mode the engine never calls the renderer's
  // `present` callback, so this is the only place we drive presentation.
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
