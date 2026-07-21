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

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <wayland-egl.h>

#include "config/common.h"

#include "backend/backend.h"
#include "egl.h"

struct wl_output;
class Display;
class TaskRunner;

#include "vsync/wayland_vsync_provider.h"

#if BUILD_COMPOSITOR
#include <memory>
#include <mutex>

#include "backend/backing_store_pool.h"
#include "backend/wayland_egl/egl_backing_store.h"
#include "backend/wayland_egl/gl_caps.h"
#include "backend/wayland_egl/gl_compositor.h"
#include "view/compositor_surface_interface.h"
#include "view/mutation_stack.h"
#include "view/present_layer_sequencer.h"
#endif

class Backend;

class Engine;

namespace ihs::hud {
class GlHud;
}  // namespace ihs::hud

class WaylandEglBackend : public Egl, public Backend {
 public:
  // Maximum damage history - for triple buffering we need to store damage for
  // last two frames.
  static constexpr int kMaxHistorySize = 10;

  // @p shell_display is the @c Display owning the wp_presentation global.
  // May be nullptr (e.g., for tests that don't construct a Display); the
  // backend then falls back to the wall-clock vsync scheduler.
  // @p enable_impeller reflects whether the engine was asked to run Impeller
  // (via the --enable-impeller switch). When true the backend disables its
  // partial-repaint path and reports full-surface damage, since that path is
  // only correct under the Skia OpenGL renderer.
  WaylandEglBackend(Display* shell_display,
                    struct wl_display* display,
                    uint32_t initial_width,
                    uint32_t initial_height,
                    bool debug_backend,
                    bool enable_impeller,
                    int buffer_size = kEglBufferSize);

#if BUILD_HUD
  // Out-of-line so the GlHud unique_ptr (incomplete here) is destroyed where
  // GlHud is complete; also tears the HUD down with the GL context current.
  ~WaylandEglBackend() override;
#endif

  /**
   * @brief Resize Flutter engine Window size
   * @param[in] index No use
   * @param[in] engine Pointer to Flutter engine
   * @param[in] width Set window width
   * @param[in] height Set window height
   * @return void
   * @relation
   * wayland
   */
  void Resize(size_t index,
              Engine* flutter_engine,
              int32_t width,
              int32_t height) override;

  /**
   * @brief Create EGL surface
   * @param[in] index No use
   * @param[in] surface Pointer to surface
   * @param[in] width Set surface width
   * @param[in] height Set surface height
   * @return void
   * @relation
   * wayland
   */
  void CreateSurface(const size_t index,
                     struct wl_surface* surface,
                     const int32_t width,
                     const int32_t height) override;

  bool TextureMakeCurrent() override;

  bool TextureClearCurrent() override;

  bool GetEglContext(BackendEglContext* out) const override;

  /**
   * @brief Get FlutterRendererConfig
   * @return FlutterRendererConfig
   * @retval Pointer to FlutterRendererConfig
   * @relation
   * wayland
   */
  FlutterRendererConfig GetRenderConfig() override;

  /**
   * @brief Get FlutterCompositor
   * @return FlutterCompositor
   * @retval Pointer to FlutterCompositor
   * @relation
   * wayland
   */
  FlutterCompositor GetCompositorConfig() override;

  /**
   * @brief Per-backend FlutterVsyncCallback.
   *
   * Returns @c &VsyncTrampoline iff (a) the compositor advertised
   * wp_presentation, (b) the announced clock_id is CLOCK_MONOTONIC,
   * and (c) @c IVI_WL_VSYNC is not @c 0. Otherwise returns nullptr and
   * Flutter falls back to its internal wall-clock scheduler.
   */
  [[nodiscard]] VsyncCallback GetVsyncCallback() const override;

  /**
   * @brief Stash the engine handle for the wp_presentation_feedback
   * path. Captured atomically because dispatch may read it from a
   * thread other than the caller (event_thread_ when the listener fires).
   */
  void SetEngineHandle(FLUTTER_API_SYMBOL(FlutterEngine) engine) override {
    engine_handle_ = engine;
    if (vsync_) {
      vsync_->SetEngine(engine_handle_, platform_task_runner_);
    }
  }

  /**
   * @brief Stash the platform task runner so @c FlutterEngineOnVsync
   * can be marshalled onto its strand. Required because the listener
   * for @c wp_presentation_feedback.presented fires on Display's
   * event_thread_, and Flutter rejects @c OnVsync from any thread
   * other than the engine's run thread.
   */
  void SetPlatformTaskRunner(TaskRunner* runner) override {
    platform_task_runner_ = runner;
    if (vsync_) {
      vsync_->SetEngine(engine_handle_, platform_task_runner_);
    }
  }

  /**
   * @brief Cancel outstanding wp_presentation_feedback objects so
   * listeners can't fire after the engine has destructed. Called from
   * @c FlutterView::~FlutterView before the engine destructs.
   */
  void StopVsyncMonitor() override;

  /**
   * @brief Release the EGL surface, wl_egl_window and GL contexts. Called
   * from @c FlutterView::~FlutterView after the engine has been stopped and
   * joined, while the wl_surface / wl_display are still alive.
   */
  void ReleaseRenderSurfaces() override;

  void UpdateSize(int _width, int _height) {
    m_initial_width = static_cast<uint32_t>(_width);
    m_initial_height = static_cast<uint32_t>(_height);
  }

#if BUILD_COMPOSITOR
  /**
   * @brief Register a platform-view surface for composition.
   *
   * Called by @c FlutterView in response to the @c flutter/platform_views
   * create message. Must be paired with @c UnregisterCompositorSurface.
   */
  void RegisterCompositorSurface(
      FlutterPlatformViewIdentifier id,
      std::shared_ptr<ICompositorSurface> surface) override;

  void UnregisterCompositorSurface(FlutterPlatformViewIdentifier id) override;

  void ResizeCompositorSurface(FlutterPlatformViewIdentifier id,
                               int32_t width,
                               int32_t height) override;
#endif

 private:
  struct wl_egl_window* m_egl_window{};
  // wl_surface backing m_egl_window. We stash it from CreateSurface so
  // wp_presentation_feedback() can mint a feedback object per commit.
  // Atomic because CreateSurface (platform thread, during init/resize)
  // and RequestPresentationFeedback (rasterizer thread) can otherwise
  // race during a window resize.
  std::atomic<struct wl_surface*> m_wl_surface{nullptr};
  uint32_t m_initial_width;
  uint32_t m_initial_height;

  // False when Impeller is the active renderer: the partial-repaint path
  // (existing-damage query + damage swap) is bypassed in favor of full-surface
  // repaint. See partial_repaint_gate.h for the rationale.
  bool m_partial_repaint_enabled;

  // Keeps track of the existing damage associated with each FBO ID.
  // Storing the rect by value (not via heap) so populate_existing_damage
  // doesn't malloc/free per frame; std::unordered_map keeps element
  // addresses stable across insertions, so handing &map[fbo_id] to the
  // engine is safe across frames.
  std::unordered_map<intptr_t, FlutterRect> m_existing_damage_map;

  // Keeps track of the most recent frame damages so that existing damage can
  // be easily computed.
  std::list<FlutterRect> m_damage_history{};

  /**
   * @brief Auxiliary function to union the damage regions comprised by two
   * FlutterRect element. It saves the result of this join in the rect variable.
   * @return void
   * @relation
   * wayland
   */
  static void JoinFlutterRect(FlutterRect* rect,
                              const FlutterRect& additional_rect);

#if BUILD_COMPOSITOR
  GlCaps m_gl_caps;
  bool m_gl_caps_probed{false};
  std::unique_ptr<GlCompositor> m_gl_compositor;
  BackingStorePool<EglFboBackingStore> m_fbo_pool;
  BackingStorePool<EglTextureBackingStore> m_texture_pool;
  PresentLayerSequencer m_sequencer;

#if BUILD_HUD
  // Debug HUD (imgui GL). Lazily created on the first present when IVI_HUD is
  // set, or on the first ToggleHud (F12). All HUD GL work runs on the raster
  // thread with the context current; rolling present-interval window feeds the
  // fps/frame stats independent of IVI_WL_PROFILE.
  std::unique_ptr<ihs::hud::GlHud> hud_;
  bool hud_enabled_{false};
  bool hud_checked_{false};
  bool hud_init_failed_{false};
  uint64_t hud_last_present_ns_{0};
  std::array<float, 60> hud_interval_ms_{};
  uint32_t hud_interval_head_{0};
  uint32_t hud_interval_count_{0};

  // Lazily create hud_ (no-op if disabled/already tried) and draw it over the
  // window's default framebuffer before the buffer swap. @layers/@count may be
  // null/0 (fast path) — then no platform-view rows are updated this frame.
  void MaybeRenderHud(const FlutterLayer** layers, size_t count);
#endif  // BUILD_HUD

  // Register/Unregister fire on the platform thread via FlutterView;
  // PresentLayers reads the map on the rasterizer thread. The mutex is
  // held only across map lookups — never across the OnPresent call
  // itself. We snapshot a shared_ptr copy under the lock before dropping
  // it, so OnPresent runs lock-free on a per-frame-stable pointer.
  mutable std::mutex m_compositor_surfaces_mu_;
  std::unordered_map<FlutterPlatformViewIdentifier,
                     std::shared_ptr<ICompositorSurface>>
      m_compositor_surfaces;

  // Holds shared ownership of backing stores while the engine has them
  // checked out. Keyed by raw pointer — the engine's @c user_data baton
  // refers back to this map for Collect.
  std::unordered_map<EglFboBackingStore*, std::shared_ptr<EglFboBackingStore>>
      m_alive_stores_;
  std::unordered_map<EglTextureBackingStore*,
                     std::shared_ptr<EglTextureBackingStore>>
      m_alive_texture_stores_;

  // One-per-backend scratch FBO used to wrap texture-subtype backing
  // stores just before compositing so the blit path in GlCompositor
  // works uniformly. Created lazily.
  GLuint m_texture_blit_fbo_{0};

  /// Lazily probe GL caps and instantiate @c GlCompositor. Must be called
  /// with the EGL context current.
  void EnsureGlCapsProbed();

  bool CreateBackingStore(const FlutterBackingStoreConfig* config,
                          FlutterBackingStore* store_out);
  bool CollectBackingStore(const FlutterBackingStore* store);
  bool PresentLayers(const FlutterLayer** layers, size_t count);

  /// Blit backing FBO into FBO 0 (window) and swap.
  bool BlitBackingStoreToWindow(const FlutterBackingStore* store);

  /// Composite a single layer's backing store onto FBO 0 at the given
  /// pixel rect. Handles both FBO and texture subtypes.
  ///
  /// When @p blend is true, the composite alpha-blends with premultiplied
  /// alpha so transparent pixels preserve underlying framebuffer content —
  /// required for overlay layers stacked on top of platform views or other
  /// backing stores.
  void CompositeLayer(const FlutterBackingStore* store,
                      GLint dst_x,
                      GLint dst_y,
                      GLsizei dst_w,
                      GLsizei dst_h,
                      bool blend = false);

  /// True when the requested size equals the view's root dimensions.
  /// Used to pick FBO vs texture subtype at create time.
  [[nodiscard]] bool IsRootSize(int32_t width, int32_t height) const {
    return width == static_cast<int32_t>(m_initial_width) &&
           height == static_cast<int32_t>(m_initial_height);
  }
#endif

  // wp_presentation-driven vsync now lives in WaylandVsyncProvider (owned by
  // Display). The backend just forwards: GetVsyncCallback gates on
  // vsync_->Usable(), VsyncTrampoline → SetVsyncBaton → vsync_->SubmitBaton,
  // RequestPresentationFeedback → vsync_->RequestFeedback,
  // StopVsyncMonitor → vsync_->Stop. The engine/runner are re-passed to the
  // provider via SetEngine when both are known.
  ivi::WaylandVsyncProvider* vsync_{nullptr};
  FLUTTER_API_SYMBOL(FlutterEngine) engine_handle_ { nullptr };
  TaskRunner* platform_task_runner_{nullptr};

  // eglSwapBuffers self-time profile (IVI_WL_PROFILE=1). This is the metric
  // that the swap-interval change moves: with interval 1 the swap blocks the
  // rasterizer on the compositor throttle, with interval 0 it returns
  // promptly. Accumulated and flushed entirely on the rasterizer thread
  // (present_with_info), so it is kept separate from profile_ — which is
  // written on Display's event_thread_ — to avoid a cross-thread data race.
  struct SwapProfile {
    uint64_t sum_ns{0};
    uint64_t max_ns{0};
    uint32_t samples{0};
    uint32_t blocked{0};  // swaps slower than kSwapBlockedNs (≈ blocked)
  };
  static constexpr uint64_t kSwapBlockedNs = 2'000'000;  // 2ms
  SwapProfile swap_profile_{};
  SwapProfile swap_session_{};

  // Record one eglSwapBuffers duration and flush a window every 60 swaps.
  // Rasterizer-thread only. No-op unless IVI_WL_PROFILE is set.
  void RecordSwapDuration(uint64_t swap_ns);

  // Mirrors DrmBackend::VsyncTrampoline. Flutter calls this with the
  // FlutterDesktopEngineState* as user_data plus an opaque baton; we
  // recover the backend instance and forward to SetVsyncBaton.
  static void VsyncTrampoline(void* user_data, intptr_t baton);

  // Forward the parked baton to the provider (which drains it on the next
  // presented/discarded, or immediately if the pipeline is idle).
  void SetVsyncBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine, intptr_t baton);

  // Per-commit feedback request — called BEFORE eglSwapBuffers (which mints
  // the wl_surface.commit the feedback binds to). Delegates to the provider.
  void RequestPresentationFeedback();
};
