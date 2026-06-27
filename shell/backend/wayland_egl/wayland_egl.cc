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
#include "logging/logging.h"

#include <wayland-egl.h>

#include <asio/post.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string_view>

#include "../gl_process_resolver.h"
#include "egl.h"
#include "engine.h"
#include "logging.h"
#include "profiling/frame_profile.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"
#include "shell/platform/homescreen/flutter_desktop_texture_registrar.h"
#include "task_runner.h"
#include "wayland/display.h"

#if BUILD_COMPOSITOR
#include <GLES2/gl2.h>
#endif

struct FlutterDesktopEngineState;

WaylandEglBackend::WaylandEglBackend(Display* shell_display,
                                     struct wl_display* display,
                                     const uint32_t initial_width,
                                     const uint32_t initial_height,
                                     const bool debug_backend,
                                     const int buffer_size)
    : Egl(display, buffer_size, debug_backend),
      Shell(),
      m_initial_width(initial_width),
      m_initial_height(initial_height) {
  if (shell_display != nullptr) {
    // The wp_presentation global + vsync feedback machinery live in the
    // Display-owned provider; the backend forwards to it. Usable() reports
    // whether the compositor advertised wp_presentation with a compatible
    // clock.
    vsync_ = shell_display->GetVsyncProvider();
    if (vsync_ != nullptr && !vsync_->Usable()) {
      spdlog::info(
          "[WaylandEglBackend] wp_presentation unusable — vsync_callback "
          "disabled, falling back to engine wall-clock scheduler");
    }
  }
}

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
    return PopulateExternalGlTextureFrame(
        state->texture_registrar.get(), texture_id, width, height, texture_out);
  };

  config.open_gl.present_with_info =
      [](void* userdata, const FlutterPresentInfo* info) -> bool {
    const auto state = static_cast<FlutterDesktopEngineState*>(userdata);
    auto* b = reinterpret_cast<WaylandEglBackend*>(
        state->view_controller->engine->GetBackend());

    // Request wp_presentation feedback before the swap so it binds to
    // THIS commit. No-op when wp_presentation isn't usable.
    b->RequestPresentationFeedback();

    static const bool profile_enabled =
        profiling::FrameProfile::Enabled("IVI_WL_PROFILE");

    // Decide the swap mechanism (and do the cheap damage bookkeeping) BEFORE
    // timing, so the measured interval reflects only the swap-call block time
    // — the quantity the swap-interval setting moves — not RectToInts / damage
    // map work.
    bool use_damage_swap = false;
    std::array<EGLint, 4> frame_rects{};
    if (info->struct_size == sizeof(FlutterPresentInfo)) {
      // Existing-damage storage is held by value in m_existing_damage_map;
      // populate_existing_damage rewrites the entry in place on every call,
      // so no per-frame free is required here.
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
          frame_rects = b->RectToInts(info->frame_damage.damage[0]);
          use_damage_swap = true;
        }
      }
    }

    const uint64_t t0 =
        profile_enabled ? LibFlutterEngine->GetCurrentTime() : 0;
    bool result;
    if (use_damage_swap) {
      // Swap buffers with frame damage.
      result = b->GetSwapBuffersWithDamage()(
          b->GetDisplay(), b->m_egl_surface,
          const_cast<int*>(frame_rects.data()), 1);
    } else {
      // Full repaint: invalid present info, no frame damage, or the
      // partial-repaint extension wasn't provided.
      result = b->SwapBuffers();
    }
    if (profile_enabled) {
      b->RecordSwapDuration(LibFlutterEngine->GetCurrentTime() - t0);
    }
    return result;
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

    // Reuse the per-fbo slot in the map. operator[] default-constructs the
    // FlutterRect on first use and returns a stable reference thereafter
    // (std::unordered_map keeps node addresses stable across insertions).
    FlutterRect& slot = b->m_existing_damage_map[fbo_id];
    slot = FlutterRect{0, 0, static_cast<double>(b->m_initial_width),
                       static_cast<double>(b->m_initial_height)};
    existing_damage->damage = &slot;

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

VsyncCallback WaylandEglBackend::GetVsyncCallback() const {
  // The provider gates on wp_presentation being advertised, its clock being
  // CLOCK_MONOTONIC-compatible, and the IVI_WL_VSYNC=0 kill-switch. When not
  // usable, Flutter falls back to its internal wall-clock scheduler.
  if (vsync_ == nullptr || !vsync_->Usable()) {
    return nullptr;
  }
  return &VsyncTrampoline;
}

void WaylandEglBackend::VsyncTrampoline(void* user_data, const intptr_t baton) {
  // user_data is the FlutterDesktopEngineState* handed to
  // FlutterEngineInitialize. Recover the engine + backend from it; if either is
  // gone (shutdown race) we drop the baton — leaking is the documented cost of
  // an unresponded baton but the only safe option once the engine's torn down.
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr || state->view_controller == nullptr ||
      state->view_controller->engine == nullptr) {
    return;
  }
  auto* engine_obj = state->view_controller->engine;
  auto* backend = dynamic_cast<WaylandEglBackend*>(engine_obj->GetBackend());
  if (backend == nullptr) {
    return;
  }
  backend->SetVsyncBaton(engine_obj->GetFlutterEngine(), baton);
}

void WaylandEglBackend::SetVsyncBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                                      const intptr_t baton) {
  if (vsync_ != nullptr) {
    vsync_->SubmitBaton(engine, baton);
  }
}

void WaylandEglBackend::RequestPresentationFeedback() {
  // Rasterizer thread, BEFORE eglSwapBuffers (which mints the wl_surface.commit
  // the feedback binds to). The provider no-ops when wp_presentation isn't
  // usable or no surface is attached yet.
  if (vsync_ != nullptr) {
    vsync_->RequestFeedback();
  }
}

void WaylandEglBackend::StopVsyncMonitor() {
  // Called from FlutterView::~FlutterView before the engine destructs. The
  // provider cancels + drains any in-flight presentation feedback and drops the
  // parked baton (an unresponded baton is a documented leak, not a crash —
  // Flutter has already begun shutdown).
  if (vsync_ != nullptr) {
    vsync_->Stop();
  }

  // Session-aggregate eglSwapBuffers self-time (IVI_WL_PROFILE=1). This is the
  // metric the swap-interval setting moves: lower mean / fewer blocked swaps
  // means the rasterizer spent less time stalled in the swap.
  if (const auto& sw = swap_session_; sw.samples > 0) {
    spdlog::info(
        "[WaylandEglBackend] swap session: n={} mean={}us max={}us "
        "blocked(>2ms)={} ({:.1f}%)",
        sw.samples, (sw.sum_ns / sw.samples) / 1000, sw.max_ns / 1000,
        sw.blocked, 100.0 * sw.blocked / static_cast<double>(sw.samples));
  }

  // NB: GPU / WSI resources (EGL surface, wl_egl_window, GL contexts,
  // EGLDisplay) are NOT torn down here. StopVsyncMonitor runs before the
  // engine is stopped, so the rasterizer thread may still be live and have a
  // context current; destroying them now races that thread. They are released
  // from ReleaseRenderSurfaces(), which FlutterView::~FlutterView calls after
  // the engine has been stopped and joined.
}

void WaylandEglBackend::ReleaseRenderSurfaces() {
  // Called from FlutterView::~FlutterView after Engine::Shutdown() has joined
  // every engine thread (so no rasterizer thread holds a context current) but
  // while m_wayland_window (the wl_surface) and m_display (the wl_display) are
  // still alive (so the EGL surface / WSI objects and the EGLDisplay are still
  // valid). This is the only safe point to release them. Idempotent via the
  // guards below plus Egl::ReleaseContexts().
#if BUILD_COMPOSITOR
  // The scratch blit FBO needs a current context; delete it before the
  // surface goes away.
  if (m_texture_blit_fbo_) {
    MakeCurrent();
    glDeleteFramebuffers(1, &m_texture_blit_fbo_);
    m_texture_blit_fbo_ = 0;
  }
#endif
  // eglDestroySurface must not run on a bound surface — unbind first.
  ClearCurrent();
  if (m_egl_surface != EGL_NO_SURFACE) {
    eglDestroySurface(GetDisplay(), m_egl_surface);
    m_egl_surface = EGL_NO_SURFACE;
  }
  if (m_egl_window != nullptr) {
    wl_egl_window_destroy(m_egl_window);
    m_egl_window = nullptr;
  }
  // Destroy the GL contexts and terminate the display now, while no engine
  // thread is alive. (Egl::~Egl would otherwise run at member-destruction
  // time, after m_display has freed the wl_display the EGLDisplay wraps.)
  ReleaseContexts();
}

void WaylandEglBackend::RecordSwapDuration(const uint64_t swap_ns) {
  const auto accumulate = [swap_ns](SwapProfile& p) {
    p.sum_ns += swap_ns;
    if (swap_ns > p.max_ns) {
      p.max_ns = swap_ns;
    }
    if (swap_ns > kSwapBlockedNs) {
      ++p.blocked;
    }
    ++p.samples;
  };
  accumulate(swap_profile_);
  accumulate(swap_session_);

  // Flush a window every 60 swaps, mirroring the frame-interval profiler.
  if (auto& w = swap_profile_; w.samples >= 60) {
    spdlog::info(
        "[WaylandEglBackend] swap profile (n={}): mean={}us max={}us "
        "blocked(>2ms)={} ({:.1f}%)",
        w.samples, (w.sum_ns / w.samples) / 1000, w.max_ns / 1000, w.blocked,
        100.0 * w.blocked / static_cast<double>(w.samples));
    w = SwapProfile{};
  }
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
  // Stash the wl_surface for per-commit wp_presentation_feedback requests.
  // EGL takes a reference internally via wl_egl_window_create — the
  // surface stays valid for the lifetime of the egl_window.
  m_wl_surface.store(surface, std::memory_order_release);
  if (vsync_ != nullptr) {
    vsync_->SetSurface(surface);
  }
  m_egl_window = wl_egl_window_create(surface, width, height);
  m_egl_surface = create_egl_surface(m_egl_window, nullptr);

  // Frame pacing is owned by the wp_presentation_feedback ->
  // FlutterEngineOnVsync path whenever it is active, so make eglSwapBuffers
  // non-blocking (swap interval 0) and avoid double-throttling the rasterizer
  // inside the swap. When that path is disabled (IVI_WL_VSYNC=0, or no
  // compatible presentation clock) the engine falls back to its wall-clock
  // scheduler and the swap interval is the only throttle, so keep the EGL
  // default of 1 there. Mirrors GetVsyncCallback()'s condition exactly so the
  // two never disagree.
  EGLint swap_interval = GetVsyncCallback() != nullptr ? 0 : 1;
  // IVI_WL_SWAP_INTERVAL=auto|0|1 forces the interval independent of the vsync
  // path, so it can be A/B'd (0 vs 1 with vsync held on) when measuring the
  // rasterizer swap-block time. "auto" (default) keeps the vsync-coupled value.
  if (const char* env = std::getenv("IVI_WL_SWAP_INTERVAL")) {
    if (std::string_view(env) == "0") {
      swap_interval = 0;
    } else if (std::string_view(env) == "1") {
      swap_interval = 1;
    }
    spdlog::info("[WaylandEglBackend] IVI_WL_SWAP_INTERVAL={} -> interval {}",
                 env, swap_interval);
  }
  MakeCurrent();
  if (eglSwapInterval(GetDisplay(), swap_interval) != EGL_TRUE) {
    spdlog::warn("[WaylandEglBackend] eglSwapInterval({}) failed",
                 swap_interval);
  } else {
    SPDLOG_DEBUG("[WaylandEglBackend] eglSwapInterval set to {}",
                 swap_interval);
  }
  ClearCurrent();
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

bool WaylandEglBackend::GetEglContext(BackendEglContext* out) const {
  if (!out) {
    return false;
  }
  out->display = GetDisplay();
  out->config = GetConfig();
  // |m_texture_context| is created with the main render context as its
  // share_context, so a plugin context created with this as its own
  // share_context transitively shares GL objects with Flutter's raster
  // context.
  out->share_context = GetTextureContext();
  return out->display != EGL_NO_DISPLAY && out->share_context != EGL_NO_CONTEXT;
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

  const auto w = static_cast<int32_t>(config->size.width);
  const auto h = static_cast<int32_t>(config->size.height);

  // Sized internal format preference — ES3 / OES_rgb8_rgba8 → sized, else
  // unsized. Same rule the backing store itself uses internally.
#if BUILD_EGL_TRANSPARENCY
  const GLenum color_internal =
      m_gl_caps.has_rgb8_rgba8 ? GL_RGBA8_OES : GL_RGBA;
#else
  const GLenum color_internal = m_gl_caps.has_rgb8_rgba8 ? GL_RGB8_OES : GL_RGB;
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
                                       GLsizei dst_h,
                                       bool blend) {
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
                                        dst_y, dst_w, dst_h, blend);
    return;
  }

  // Texture subtype: the engine wrapped our texture in its own private FBO
  // during rendering. For the compositor we either (a) attach the texture to
  // a scratch FBO and use the blit path, or (b) use the quad path directly.
  // Favour the blit path when available since it's a fixed-function copy.
  auto* tex = baton->tex_store;
  if (!blend && m_gl_caps.has_blit_framebuffer) {
    if (!m_texture_blit_fbo_) {
      glGenFramebuffers(1, &m_texture_blit_fbo_);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_texture_blit_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           tex->Texture(), 0);
    m_gl_compositor->CompositeToDefault(m_texture_blit_fbo_, tex->Texture(),
                                        tex->Width(), tex->Height(), dst_x,
                                        dst_y, dst_w, dst_h, blend);
    // Leave the attachment in place — rebinding before next composite is
    // cheaper than detaching every frame.
  } else {
    // Quad path doesn't need a scratch FBO; pass 0. This is the only path
    // that supports alpha blending.
    m_gl_compositor->CompositeToDefault(0, tex->Texture(), tex->Width(),
                                        tex->Height(), dst_x, dst_y, dst_w,
                                        dst_h, blend);
  }
}

bool WaylandEglBackend::BlitBackingStoreToWindow(
    const FlutterBackingStore* store) {
  auto* baton = static_cast<StoreBaton*>(store->user_data);
  if (!baton) {
    spdlog::error("WaylandEglBackend: present layer missing baton");
    return false;
  }
  const GLsizei w = (baton->kind == StoreKind::Fbo) ? baton->fbo_store->Width()
                                                    : baton->tex_store->Width();
  const GLsizei h = (baton->kind == StoreKind::Fbo)
                        ? baton->fbo_store->Height()
                        : baton->tex_store->Height();
  CompositeLayer(store, 0, 0, w, h);
  return SwapBuffers();
}

bool WaylandEglBackend::PresentLayers(const FlutterLayer** layers,
                                      size_t count) {
  // Request wp_presentation feedback before the swap so it binds to THIS
  // commit. Both the fast path (BlitBackingStoreToWindow) and the general
  // path below end in eglSwapBuffers, and both want feedback — do it once
  // here at the entry so the request and the commit are not interleaved
  // with anything else on the wl_display proxy queue.
  RequestPresentationFeedback();

  // Fast path: a single Flutter-rendered layer, no platform views.
  if (count == 1 && layers[0]->type == kFlutterLayerContentTypeBackingStore &&
      layers[0]->backing_store) {
    return BlitBackingStoreToWindow(layers[0]->backing_store);
  }

  // General path: Wayland subsurface Z-order is reconciled by the sequencer;
  // each Flutter layer is blitted onto the window, each platform view is
  // dispatched to its registered ICompositorSurface.
  m_sequencer.Present(
      layers, count, nullptr, [this](FlutterPlatformViewIdentifier id) {
        // PVs may choose the wl_subsurface route (sequencer)
        // or the ICompositorSurface texture route (below).
        // Only warn when *neither* is registered.
        std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
        if (m_compositor_surfaces.find(id) == m_compositor_surfaces.end()) {
          spdlog::warn(
              "EGL compositor: platform view {} has no "
              "registered subsurface or compositor surface",
              id);
        }
      });

  // Clear the window backbuffer before compositing. Without this, stale
  // pixels from the driver's swapchain rotation remain wherever no opaque
  // layer covers them — visible as trails behind a moving platform view
  // when the Flutter overlay is mostly transparent.
  //
  // Alpha=0 so regions left uncovered by any composited layer fall through
  // to the Wayland compositor. The surface has no wl_surface_set_opaque_region
  // set by default, and the EGL config has EGL_ALPHA_SIZE=8, so per-pixel
  // alpha is honoured downstream.
  //
  // Skip the clear when the first composited layer is a window-sized
  // BackingStore at offset (0,0): its blend=false path overwrites the
  // entire FBO 0, so the upfront clear's pixels are immediately discarded.
  // PlatformView layers ahead of any BackingStore disqualify the skip
  // because their cutouts may not be covered by any later layer.
  bool first_layer_covers_window = false;
  for (size_t i = 0; i < count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (!layer) {
      continue;
    }
    if (layer->type == kFlutterLayerContentTypeBackingStore &&
        layer->backing_store) {
      first_layer_covers_window =
          layer->offset.x == 0.0 && layer->offset.y == 0.0 &&
          static_cast<uint32_t>(layer->size.width) == m_initial_width &&
          static_cast<uint32_t>(layer->size.height) == m_initial_height;
      break;
    }
    if (layer->type == kFlutterLayerContentTypePlatformView) {
      break;
    }
  }

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (!first_layer_covers_window) {
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
  }

  // Enter frame-batched compositor mode so each quad-path composite skips
  // the program/VBO/attrib setup that's invariant across layers within a
  // frame. Matched by EndFrame after the loop. Probe caps up front so
  // m_gl_compositor is constructed.
  EnsureGlCapsProbed();
  m_gl_compositor->BeginFrame();

  bool ok = true;
  // Engine emits layers bottom-to-top. The first composited layer lands on
  // the window backbuffer with an opaque copy; every subsequent layer is an
  // overlay whose transparent pixels must preserve what's underneath, so it
  // has to alpha-blend rather than overwrite. Without this, a Flutter
  // overlay backing store (mostly alpha-0 around platform view cutouts)
  // paints black over the platform view textures we just drew.
  bool composited_any = false;
  for (size_t i = 0; i < count; ++i) {
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
      CompositeLayer(layer->backing_store, dx, dy, dw, dh, blend);
      composited_any = true;
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
      // Snapshot the shared_ptr under the lock, drop the lock before the
      // OnPresent callback. Holding the mutex across plugin GL work would
      // serialise register/unregister with rendering.
      std::shared_ptr<ICompositorSurface> surface_sp;
      {
        std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
        const auto it =
            m_compositor_surfaces.find(layer->platform_view->identifier);
        if (it != m_compositor_surfaces.end()) {
          surface_sp = it->second;
        }
      }
      if (surface_sp) {
        auto& surface = *surface_sp;
        ok = surface.OnPresent(layer) && ok;
        // If the plugin exposed a GL texture, composite it onto FBO 0 at
        // the layer's pixel rect. Plugins that handle their own
        // presentation (legacy wl_subsurface path, etc.) return 0 here.
        if (const auto tex = surface.GetGlTextureName(); tex != 0) {
          EnsureGlCapsProbed();
          const auto sw = surface.GetGlTextureWidth();
          const auto sh = surface.GetGlTextureHeight();
          const auto dx = static_cast<GLint>(layer->offset.x);
          const auto dw = static_cast<GLsizei>(layer->size.width);
          const auto dh = static_cast<GLsizei>(layer->size.height);
          // layer->offset.y is Flutter top-left-origin; the default GL
          // framebuffer is bottom-left-origin. Convert so the plugin texture
          // lands where Flutter's overlay markers are drawn.
          const auto dy = static_cast<GLint>(m_initial_height) -
                          static_cast<GLint>(layer->offset.y) - dh;
          // Orientation: FBO 0 on Wayland uses standard GL window NDC.
          // GL-native plugin textures (bottom-first memory) sample correctly
          // with no flip. Plugins that produce top-first textures (NV12
          // dmabuf, pre-flipped YUV shader) opt into a sampler V-invert by
          // overriding ICompositorSurface::TextureIsTopFirst() → true.
          const bool flip_y = surface.TextureIsTopFirst();
          if (!blend && m_gl_caps.has_blit_framebuffer) {
            if (!m_texture_blit_fbo_) {
              glGenFramebuffers(1, &m_texture_blit_fbo_);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, m_texture_blit_fbo_);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tex, 0);
            m_gl_compositor->CompositeToDefault(m_texture_blit_fbo_, tex, sw,
                                                sh, dx, dy, dw, dh, blend,
                                                flip_y);
          } else {
            m_gl_compositor->CompositeToDefault(0, tex, sw, sh, dx, dy, dw, dh,
                                                blend, flip_y);
          }
          composited_any = true;
        }
      }
    }
  }

  m_gl_compositor->EndFrame();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return SwapBuffers() && ok;
}

void WaylandEglBackend::RegisterCompositorSurface(
    FlutterPlatformViewIdentifier id,
    std::shared_ptr<ICompositorSurface> surface) {
  std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
  m_compositor_surfaces[id] = std::move(surface);
}

void WaylandEglBackend::UnregisterCompositorSurface(
    FlutterPlatformViewIdentifier id) {
  std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
  m_compositor_surfaces.erase(id);
}

void WaylandEglBackend::ResizeCompositorSurface(
    FlutterPlatformViewIdentifier id,
    int32_t width,
    int32_t height) {
  // Snapshot the shared_ptr under the lock, drop the lock before calling
  // OnResize — the callback may re-enter (e.g., post a render task) and
  // should not hold the registry mutex.
  std::shared_ptr<ICompositorSurface> surface;
  {
    std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
    const auto it = m_compositor_surfaces.find(id);
    if (it == m_compositor_surfaces.end()) {
      return;
    }
    surface = it->second;
  }
  if (surface) {
    surface->OnResize(width, height);
  }
}

#endif  // BUILD_COMPOSITOR
