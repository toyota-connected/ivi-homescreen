/*
 * Copyright 2020-2022 Toyota Connected North America
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

#include "wayland_egl.h"

#include <wayland-egl.h>

#include "../gl_process_resolver.h"
#include "egl.h"
#include "engine.h"
#include "logging.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"

#if BUILD_COMPOSITOR
#include <GLES2/gl2.h>
#endif

struct FlutterDesktopEngineState;

WaylandEglBackend::WaylandEglBackend(struct wl_display* display,
                                     const uint32_t initial_width,
                                     const uint32_t initial_height,
                                     const bool debug_backend,
                                     const int buffer_size)
    : Egl(display, buffer_size, debug_backend),
      Backend(),
      m_initial_width(initial_width),
      m_initial_height(initial_height) {}

FlutterRendererConfig WaylandEglBackend::GetRenderConfig() {
  FlutterRendererConfig config{};
  config.type = kOpenGL;
  config.open_gl.struct_size = sizeof(FlutterOpenGLRendererConfig);
  config.open_gl.make_current = [](void* user_data) -> bool {
    const auto state = static_cast<FlutterDesktopEngineState*>(user_data);
    return reinterpret_cast<WaylandEglBackend*>(
               state->view_controller->engine->GetBackend())
        ->MakeCurrent();
  };

  config.open_gl.clear_current = [](void* user_data) -> bool {
    const auto state = static_cast<FlutterDesktopEngineState*>(user_data);
    return reinterpret_cast<WaylandEglBackend*>(
               state->view_controller->engine->GetBackend())
        ->ClearCurrent();
  };

  config.open_gl.fbo_callback = [](void* /* user_data */) -> uint32_t {
    return 0;  // FBO0
  };

  config.open_gl.make_resource_current = [](void* user_data) -> bool {
    const auto state = static_cast<FlutterDesktopEngineState*>(user_data);
    return reinterpret_cast<WaylandEglBackend*>(
               state->view_controller->engine->GetBackend())
        ->MakeResourceCurrent();
  };

  config.open_gl.fbo_reset_after_present = false;

  config.open_gl.gl_proc_resolver = [](void* /* userdata */,
                                       const char* name) -> void* {
    return GlProcessResolver::GetInstance().process_resolver(name);
  };

  config.open_gl.gl_external_texture_frame_callback =
      [](void* userdata, const int64_t texture_id, const size_t width,
         const size_t height, FlutterOpenGLTexture* texture_out) -> bool {
    const auto state = static_cast<FlutterDesktopEngineState*>(userdata);
    auto& texture_registry = state->texture_registrar->texture_registry;
    const auto it =
        std::find_if(std::begin(texture_registry), std::end(texture_registry),
                     [&texture_id](auto&& p) { return p.first == texture_id; });
    // texture not found in registry
    if (it == std::end(texture_registry))
      return false;
    const auto& target = texture_registry[texture_id];
    *texture_out = {.target = target->target,
                    .name = target->name,
                    .format = target->format,
                    .user_data = target->release_context,
                    .destruction_callback = target->release_callback,
                    .width = target->width,
                    .height = target->height};
    target->visible_width = width;
    target->visible_width = height;
    return true;
  };

  config.open_gl.present_with_info =
      [](void* userdata, const FlutterPresentInfo* info) -> bool {
    const auto state = static_cast<FlutterDesktopEngineState*>(userdata);
    auto* b = reinterpret_cast<WaylandEglBackend*>(
        state->view_controller->engine->GetBackend());

    // Full swap if FlutterPresentInfo is invalid
    if (info->struct_size != sizeof(FlutterPresentInfo)) {
      return b->SwapBuffers();
    }

    // Free the existing damage that was allocated to this frame.
    if (b->m_existing_damage_map[info->fbo_id] != nullptr) {
      free(b->m_existing_damage_map[info->fbo_id]);
      b->m_existing_damage_map[info->fbo_id] = nullptr;
    }

    if (b->GetSetDamageRegion()) {
      // Set the buffer damage as the damage region.
      auto buffer_rects = b->RectToInts(info->buffer_damage.damage[0]);
      b->GetSetDamageRegion()(b->GetDisplay(), b->m_egl_surface,
                              buffer_rects.data(), 1);
    }

    if (info->frame_damage.damage) {
      // Add frame damage to damage history
      b->m_damage_history.push_back(info->frame_damage.damage[0]);
      if (b->m_damage_history.size() > kMaxHistorySize) {
        b->m_damage_history.pop_front();
      }

      if (b->GetSwapBuffersWithDamage()) {
        // Swap buffers with frame damage.
        const auto frame_rects = b->RectToInts(info->frame_damage.damage[0]);
        return b->GetSwapBuffersWithDamage()(
            b->GetDisplay(), b->m_egl_surface,
            const_cast<int*>(frame_rects.data()), 1);
      }
    }
    // If the required extensions for partial repaint were not
    // provided, do full repaint.
    return b->SwapBuffers();
  };

  config.open_gl.populate_existing_damage =
      [](void* userdata, const intptr_t fbo_id,
         FlutterDamage* existing_damage) -> void {
    const auto state = static_cast<FlutterDesktopEngineState*>(userdata);
    auto* b = reinterpret_cast<WaylandEglBackend*>(
        state->view_controller->engine->GetBackend());
    // Given the FBO age, create existing damage region by joining
    // all frame damages since FBO was last used
    EGLint age;
    if (b->HasExtBufferAge()) {
      eglQuerySurface(b->GetDisplay(), b->m_egl_surface, EGL_BUFFER_AGE_EXT,
                      &age);
    } else {
      age = 4;  // Virtually no driver should have a swap chain
                // length > 4.
    }

    existing_damage->num_rects = 1;

    // Allocate the array of rectangles for the existing damage.
    b->m_existing_damage_map[fbo_id] = static_cast<FlutterRect*>(
        malloc(sizeof(FlutterRect) * existing_damage->num_rects));
    b->m_existing_damage_map[fbo_id][0] =
        FlutterRect{0, 0, static_cast<double>(b->m_initial_width),
                    static_cast<double>(b->m_initial_height)};
    existing_damage->damage = b->m_existing_damage_map[fbo_id];

    if (age > 1) {
      --age;
      // join up to (age - 1) last rects from damage history
      for (auto i = b->m_damage_history.rbegin();
           i != b->m_damage_history.rend() && age > 0; ++i, --age) {
        if (i == b->m_damage_history.rbegin()) {
          if (i != b->m_damage_history.rend()) {
            existing_damage->damage[0] = {i->left, i->top, i->right, i->bottom};
          }
        } else {
          JoinFlutterRect(&(existing_damage->damage[0]), *i);
        }
      }
    }
  };

  return config;
}

FlutterCompositor WaylandEglBackend::GetCompositorConfig() {
  FlutterCompositor compositor{};
  compositor.struct_size = sizeof(FlutterCompositor);
  compositor.user_data = this;

#if BUILD_COMPOSITOR
  // Engine reuses backing stores across frames when sizes match; the pool
  // mirrors that, so we allow caching.
  compositor.avoid_backing_store_cache = false;

  compositor.create_backing_store_callback =
      [](const FlutterBackingStoreConfig* config, FlutterBackingStore* out,
         void* ud) -> bool {
    return static_cast<WaylandEglBackend*>(ud)->CreateBackingStore(config, out);
  };

  compositor.collect_backing_store_callback =
      [](const FlutterBackingStore* store, void* ud) -> bool {
    return static_cast<WaylandEglBackend*>(ud)->CollectBackingStore(store);
  };

  compositor.present_layers_callback = [](const FlutterLayer** layers,
                                          size_t count, void* ud) -> bool {
    return static_cast<WaylandEglBackend*>(ud)->PresentLayers(layers, count);
  };
#else
  compositor.avoid_backing_store_cache = true;
#endif

  return compositor;
}

void WaylandEglBackend::Resize(size_t /* index */,
                               Engine* flutter_engine,
                               const int32_t width,
                               const int32_t height) {
  if (m_egl_window) {
    if (flutter_engine) {
      const auto result = flutter_engine->SetWindowSize(
          static_cast<size_t>(height), static_cast<size_t>(width));
      if (result != kSuccess) {
        spdlog::error("Failed to set Flutter Engine Window Size");
      }
    }
    UpdateSize(width, height);
    wl_egl_window_resize(m_egl_window, width, height, 0, 0);
  }
}

void WaylandEglBackend::CreateSurface(size_t /* index */,
                                      wl_surface* surface,
                                      int32_t width,
                                      int32_t height) {
  UpdateSize(width, height);
  m_egl_window = wl_egl_window_create(surface, width, height);
  m_egl_surface = create_egl_surface(m_egl_window, nullptr);
}

#if 0  // TODO
int64_t WaylandEglBackend::AddTexture() {
  MakeTextureCurrent();

  /// Create Texture
  GLuint texture_id;
  glGenTextures(1, &texture_id);
  SPDLOG_DEBUG("RegisterExternalTexture: {}", texture_id);

  /// check for danglers
  for (const auto& it : m_texture_registry) {
    if (it.first == texture_id) {
      DestroyTexture(texture_id);
      break;
    }
  }

  std::scoped_lock<std::mutex> lock(m_texture_mutex);
  m_texture_registry[texture_id] = std::make_unique<GL_TEXTURE_2D_DESC>();

  return texture_id;
}

void WaylandEglBackend::DestroyTexture(int64_t texture_id) {
  std::scoped_lock<std::mutex> lock(m_texture_mutex);
  m_texture_registry[texture_id].reset();
  m_texture_registry.erase(texture_id);
}

int64_t WaylandEglBackend::RegisterExternalTexture(
    FlutterDesktopTextureRegistrarRef /* texture_registrar */,
    const FlutterDesktopTextureInfo* texture_info) {
  int64_t result = -1;

  if (texture_info->type == kFlutterDesktopPixelBufferTexture) {
    spdlog::error("RegisterExternalTexture: Pixel Buffer not implemented yet");
  } else if (texture_info->type == kFlutterDesktopGpuSurfaceTexture) {
    auto surface_texture = texture_info->gpu_surface_config;
    if (surface_texture.type != kFlutterDesktopGpuSurfaceTypeGlTexture2D) {
      spdlog::error(
          "RegisterExternalTexture: kFlutterDesktopGpuSurfaceTypeGlTexture2D "
          "is only supported at this time");
      return result;
    }

    return AddTexture();
  }
  return result;
}

void WaylandEglBackend::UnregisterExternalTexture(
    FlutterDesktopTextureRegistrarRef texture_registrar,
    int64_t texture_id,
    void (*callback)(void* user_data),
    void* callback_context) {
  MakeTextureCurrent();
  // tear down texture_id here
  std::scoped_lock<std::mutex> lock(m_texture_mutex);
  m_texture_registry[texture_id] = std::make_unique<GL_TEXTURE_2D_DESC>();
  callback(callback_context);
  DestroyTexture(texture_id);
}

#endif  // TODO

bool WaylandEglBackend::TextureMakeCurrent() {
  return MakeTextureCurrent();
}

bool WaylandEglBackend::TextureClearCurrent() {
  return ClearCurrent();
}

void WaylandEglBackend::JoinFlutterRect(FlutterRect* rect,
                                        const FlutterRect& additional_rect) {
  rect->left = std::min(rect->left, additional_rect.left);
  rect->top = std::min(rect->top, additional_rect.top);
  rect->right = std::max(rect->right, additional_rect.right);
  rect->bottom = std::max(rect->bottom, additional_rect.bottom);
}

#if BUILD_COMPOSITOR

namespace {
// user_data slot for the FlutterOpenGLBackingStore union. `kind` tags which
// pool owns the raw pointer so collect_backing_store_callback can return it.
enum class StoreKind : uint8_t { Fbo, Texture };

struct StoreBaton {
  WaylandEglBackend* backend;
  StoreKind kind;
  union {
    EglFboBackingStore* fbo_store;
    EglTextureBackingStore* tex_store;
  };
};
}  // namespace

void WaylandEglBackend::EnsureGlCapsProbed() {
  if (m_gl_caps_probed) {
    return;
  }
  m_gl_caps.Probe();
  m_gl_compositor = std::make_unique<GlCompositor>(&m_gl_caps);
  m_gl_caps_probed = true;
}

bool WaylandEglBackend::CreateBackingStore(
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* store_out) {
  EnsureGlCapsProbed();

  const int32_t w = static_cast<int32_t>(config->size.width);
  const int32_t h = static_cast<int32_t>(config->size.height);

  // Sized internal format preference — ES3 / OES_rgb8_rgba8 → sized, else
  // unsized. Same rule the backing store itself uses internally.
#if BUILD_EGL_TRANSPARENCY
  const GLenum color_internal =
      m_gl_caps.has_rgb8_rgba8 ? GL_RGBA8_OES : GL_RGBA;
#else
  const GLenum color_internal =
      m_gl_caps.has_rgb8_rgba8 ? GL_RGB8_OES : GL_RGB;
#endif

  store_out->struct_size = sizeof(FlutterBackingStore);
  store_out->type = kFlutterBackingStoreTypeOpenGL;

  // Stack-position heuristic: a store request whose dimensions match the
  // view's root size is the root Flutter layer — use an FBO we manage. Any
  // smaller request is an overlay; we hand the engine a bare texture and let
  // it wrap its own private FBO around it (saves us allocating depth/stencil
  // for overlays that typically don't need it, and the texture name is
  // directly sampleable by plugins that want to effect the overlay).
  if (IsRootSize(w, h)) {
    auto store = m_fbo_pool.Acquire(w, h, &m_gl_caps);
    auto* baton = new StoreBaton{this, StoreKind::Fbo, {}};
    baton->fbo_store = store.get();

    store_out->user_data = baton;
    store_out->open_gl.type = kFlutterOpenGLTargetTypeFramebuffer;
    store_out->open_gl.framebuffer.target = color_internal;
    store_out->open_gl.framebuffer.name = store->Framebuffer();
    store_out->open_gl.framebuffer.user_data = baton;
    store_out->open_gl.framebuffer.destruction_callback = [](void*) {};

    EglFboBackingStore* key = store.get();
    m_alive_stores_[key] = std::move(store);
    return true;
  }

  auto store = m_texture_pool.Acquire(w, h, &m_gl_caps);
  auto* baton = new StoreBaton{this, StoreKind::Texture, {}};
  baton->tex_store = store.get();

  store_out->user_data = baton;
  store_out->open_gl.type = kFlutterOpenGLTargetTypeTexture;
  store_out->open_gl.texture.target = GL_TEXTURE_2D;
  store_out->open_gl.texture.name = store->Texture();
  store_out->open_gl.texture.format = color_internal;
  store_out->open_gl.texture.user_data = baton;
  store_out->open_gl.texture.destruction_callback = [](void*) {};
  store_out->open_gl.texture.width = static_cast<size_t>(w);
  store_out->open_gl.texture.height = static_cast<size_t>(h);

  EglTextureBackingStore* key = store.get();
  m_alive_texture_stores_[key] = std::move(store);
  return true;
}

bool WaylandEglBackend::CollectBackingStore(const FlutterBackingStore* store) {
  auto* baton = static_cast<StoreBaton*>(store->user_data);
  if (!baton) {
    return false;
  }
  switch (baton->kind) {
    case StoreKind::Fbo: {
      const auto it = m_alive_stores_.find(baton->fbo_store);
      if (it != m_alive_stores_.end()) {
        m_fbo_pool.Release(std::move(it->second));
        m_alive_stores_.erase(it);
      }
      break;
    }
    case StoreKind::Texture: {
      const auto it = m_alive_texture_stores_.find(baton->tex_store);
      if (it != m_alive_texture_stores_.end()) {
        m_texture_pool.Release(std::move(it->second));
        m_alive_texture_stores_.erase(it);
      }
      break;
    }
  }
  delete baton;
  return true;
}

void WaylandEglBackend::CompositeLayer(const FlutterBackingStore* store,
                                       GLint dst_x,
                                       GLint dst_y,
                                       GLsizei dst_w,
                                       GLsizei dst_h) {
  auto* baton = static_cast<StoreBaton*>(store->user_data);
  if (!baton) {
    spdlog::error("WaylandEglBackend: present layer missing baton");
    return;
  }

  EnsureGlCapsProbed();

  if (baton->kind == StoreKind::Fbo) {
    auto* fbo = baton->fbo_store;
    m_gl_compositor->CompositeToDefault(fbo->Framebuffer(), fbo->ColorTexture(),
                                        fbo->Width(), fbo->Height(), dst_x,
                                        dst_y, dst_w, dst_h);
    return;
  }

  // Texture subtype: the engine wrapped our texture in its own private FBO
  // during rendering. For the compositor we either (a) attach the texture to
  // a scratch FBO and use the blit path, or (b) use the quad path directly.
  // Favour the blit path when available since it's a fixed-function copy.
  auto* tex = baton->tex_store;
  if (m_gl_caps.has_blit_framebuffer) {
    if (!m_texture_blit_fbo_) {
      glGenFramebuffers(1, &m_texture_blit_fbo_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_texture_blit_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex->Texture(), 0);
    m_gl_compositor->CompositeToDefault(m_texture_blit_fbo_, tex->Texture(),
                                        tex->Width(), tex->Height(), dst_x,
                                        dst_y, dst_w, dst_h);
    // Leave the attachment in place — rebinding before next composite is
    // cheaper than detaching every frame.
  } else {
    // Quad path doesn't need an FBO; pass 0.
    m_gl_compositor->CompositeToDefault(0, tex->Texture(), tex->Width(),
                                        tex->Height(), dst_x, dst_y, dst_w,
                                        dst_h);
  }
}

bool WaylandEglBackend::BlitBackingStoreToWindow(
    const FlutterBackingStore* store) {
  auto* baton = static_cast<StoreBaton*>(store->user_data);
  if (!baton) {
    spdlog::error("WaylandEglBackend: present layer missing baton");
    return false;
  }
  const GLsizei w = (baton->kind == StoreKind::Fbo)
                        ? baton->fbo_store->Width()
                        : baton->tex_store->Width();
  const GLsizei h = (baton->kind == StoreKind::Fbo)
                        ? baton->fbo_store->Height()
                        : baton->tex_store->Height();
  CompositeLayer(store, 0, 0, w, h);
  return SwapBuffers();
}

bool WaylandEglBackend::PresentLayers(const FlutterLayer** layers,
                                      size_t count) {
  // Fast path: a single Flutter-rendered layer, no platform views.
  if (count == 1 &&
      layers[0]->type == kFlutterLayerContentTypeBackingStore &&
      layers[0]->backing_store) {
    return BlitBackingStoreToWindow(layers[0]->backing_store);
  }

  // General path: Wayland subsurface Z-order is reconciled by the sequencer;
  // each Flutter layer is blitted onto the window, each platform view is
  // dispatched to its registered ICompositorSurface.
  m_sequencer.Present(layers, count, nullptr,
                      [](FlutterPlatformViewIdentifier id) {
                        spdlog::warn(
                            "EGL compositor: platform view {} has no "
                            "registered subsurface",
                            id);
                      });

  bool ok = true;
  for (size_t i = 0; i < count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (!layer) {
      continue;
    }
    if (layer->type == kFlutterLayerContentTypeBackingStore &&
        layer->backing_store) {
      // For multi-layer: composite each backing store onto FBO 0 at its
      // layer offset. Clipping and mutation handling arrives with plugin
      // integration in Phase 4.
      const auto dx = static_cast<GLint>(layer->offset.x);
      const auto dy = static_cast<GLint>(layer->offset.y);
      const auto dw = static_cast<GLsizei>(layer->size.width);
      const auto dh = static_cast<GLsizei>(layer->size.height);
      CompositeLayer(layer->backing_store, dx, dy, dw, dh);
    } else if (layer->type == kFlutterLayerContentTypePlatformView &&
               layer->platform_view) {
      const auto composed = MutationStack::Compose(layer->platform_view);
      if (composed.NeedsPluginComposite()) {
        spdlog::debug(
            "EGL compositor: platform view {} has non-trivial mutations "
            "(opacity={:.3f} rounded={} perspective={} axis_aligned={}); "
            "plugin OnPresent must apply them.",
            layer->platform_view->identifier, composed.opacity,
            composed.has_rounded_clip, composed.has_perspective,
            composed.IsAxisAligned());
      }
      const auto it =
          m_compositor_surfaces.find(layer->platform_view->identifier);
      if (it != m_compositor_surfaces.end() && it->second) {
        auto& surface = *it->second;
        ok = surface.OnPresent(layer) && ok;
        // If the plugin exposed a GL texture, composite it onto FBO 0 at
        // the layer's pixel rect. Plugins that handle their own
        // presentation (legacy wl_subsurface path, etc.) return 0 here.
        if (const auto tex = surface.GetGlTextureName(); tex != 0) {
          EnsureGlCapsProbed();
          const auto sw = surface.GetGlTextureWidth();
          const auto sh = surface.GetGlTextureHeight();
          const auto dx = static_cast<GLint>(layer->offset.x);
          const auto dy = static_cast<GLint>(layer->offset.y);
          const auto dw = static_cast<GLsizei>(layer->size.width);
          const auto dh = static_cast<GLsizei>(layer->size.height);
          if (m_gl_caps.has_blit_framebuffer) {
            if (!m_texture_blit_fbo_) {
              glGenFramebuffers(1, &m_texture_blit_fbo_);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, m_texture_blit_fbo_);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tex, 0);
            m_gl_compositor->CompositeToDefault(m_texture_blit_fbo_, tex, sw,
                                                sh, dx, dy, dw, dh);
          } else {
            m_gl_compositor->CompositeToDefault(0, tex, sw, sh, dx, dy, dw,
                                                dh);
          }
        }
      }
    }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return SwapBuffers() && ok;
}

void WaylandEglBackend::RegisterCompositorSurface(
    FlutterPlatformViewIdentifier id,
    std::shared_ptr<ICompositorSurface> surface) {
  m_compositor_surfaces[id] = std::move(surface);
}

void WaylandEglBackend::UnregisterCompositorSurface(
    FlutterPlatformViewIdentifier id) {
  m_compositor_surfaces.erase(id);
}

void WaylandEglBackend::ResizeCompositorSurface(
    FlutterPlatformViewIdentifier id,
    int32_t width,
    int32_t height) {
  const auto it = m_compositor_surfaces.find(id);
  if (it != m_compositor_surfaces.end() && it->second) {
    it->second->OnResize(width, height);
  }
}

WaylandEglBackend::~WaylandEglBackend() {
  // Pools flush themselves; the scratch blit FBO needs explicit cleanup
  // while the EGL context from the base class is still current.
  if (m_texture_blit_fbo_) {
    glDeleteFramebuffers(1, &m_texture_blit_fbo_);
    m_texture_blit_fbo_ = 0;
  }
}

#endif  // BUILD_COMPOSITOR
