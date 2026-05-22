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

#include <atomic>
#include <cstdint>
#include <ctime>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <wayland-egl.h>

#include "config/common.h"

#include "backend/backend.h"
#include "egl.h"

struct wl_output;
struct wp_presentation;
struct wp_presentation_feedback;
class Display;
class TaskRunner;

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

class WaylandEglBackend : public Egl, public Backend {
 public:
  // Maximum damage history - for triple buffering we need to store damage for
  // last two frames.
  static constexpr int kMaxHistorySize = 10;

  // @p shell_display is the @c Display owning the wp_presentation global.
  // May be nullptr (e.g., for tests that don't construct a Display); the
  // backend then falls back to the wall-clock vsync scheduler.
  WaylandEglBackend(Display* shell_display,
                    struct wl_display* display,
                    uint32_t initial_width,
                    uint32_t initial_height,
                    bool debug_backend,
                    int buffer_size = kEglBufferSize);

#if BUILD_COMPOSITOR
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
    engine_handle_.store(engine, std::memory_order_release);
  }

  /**
   * @brief Stash the platform task runner so @c FlutterEngineOnVsync
   * can be marshalled onto its strand. Required because the listener
   * for @c wp_presentation_feedback.presented fires on Display's
   * event_thread_, and Flutter rejects @c OnVsync from any thread
   * other than the engine's run thread.
   */
  void SetPlatformTaskRunner(TaskRunner* runner) override {
    platform_task_runner_.store(runner, std::memory_order_release);
  }

  /**
   * @brief Cancel outstanding wp_presentation_feedback objects so
   * listeners can't fire after the engine has destructed. Called from
   * @c FlutterView::~FlutterView before the engine destructs.
   */
  void StopVsyncMonitor() override;

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

  // Keeps track of the existing damage associated with each FBO ID
  std::unordered_map<intptr_t, FlutterRect*> m_existing_damage_map;

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

  // wp_presentation-driven vsync plumbing.
  //
  // The producer (FlutterEngine.vsync_callback → VsyncTrampoline) parks
  // a baton in vsync_baton_; the consumer (on_feedback_presented running
  // on Display's event_thread_) exchanges it back and posts OnVsync onto
  // the platform task runner's strand. Mirrors DrmBackend's atomic baton
  // dance.
  std::atomic<intptr_t> vsync_baton_{0};
  std::atomic<FLUTTER_API_SYMBOL(FlutterEngine)> engine_handle_{nullptr};
  std::atomic<TaskRunner*> platform_task_runner_{nullptr};

  // Set when a wp_presentation_feedback has been requested for a commit
  // but the compositor has not yet emitted presented/discarded. The idle-
  // wake kick path checks this to decide whether to drain the baton
  // inline (no feedback in flight → pipeline idle) or park it (let the
  // upcoming presented event drive OnVsync).
  std::atomic<bool> feedback_pending_{false};

  // Owned wp_presentation_feedback objects awaiting presented/discarded.
  // Display's event_thread_ drives wl_display_dispatch, so listener
  // callbacks fire there while PresentLayers (rasterizer thread) creates
  // new feedback objects and StopVsyncMonitor (main thread, ~FlutterView)
  // drains at shutdown. m_feedback_mu_ serialises all three.
  std::mutex m_feedback_mu_;
  std::vector<struct wp_presentation_feedback*> feedback_in_flight_;

  // wp_presentation global + announced clock domain. Populated from
  // Display::GetWpPresentation()/GetPresentationClockId() at backend
  // construction. clock_compatible_ becomes true only when
  // presentation_clock_id_ == CLOCK_MONOTONIC, which matches
  // FlutterEngineGetCurrentTime's domain — otherwise we refuse
  // vsync_callback and fall back to the wall-clock scheduler.
  struct wp_presentation* wp_presentation_{nullptr};
  clockid_t presentation_clock_id_{CLOCK_MONOTONIC};
  bool clock_compatible_{false};

  // Last refresh period reported by wp_presentation_feedback.presented.
  // Defaults to ~60Hz so the frame_target_time math has a reasonable
  // seed for the first OnVsync call before any feedback has fired.
  // Atomic because the writer (on_feedback_presented on event_thread_)
  // races with the reader (PostOnVsync, called from rasterizer or
  // engine threads via SetVsyncBaton's idle-kick).
  std::atomic<uint64_t> last_refresh_ns_{16'666'667};

  // Per-frame cadence profile (IVI_WL_PROFILE=1). Updated by
  // on_feedback_presented / on_feedback_discarded — both fire on
  // Display's event_thread_, so a single non-atomic block of counters
  // is fine (no concurrent writer).
  //
  // Buckets categorize per-frame inter-presented intervals at a 60Hz
  // baseline (one vblank ≈ 16.67ms). Lets the README produce the same
  // shape of histogram the DRM benchmark uses.
  struct FrameProfile {
    uint64_t last_presented_ns{0};
    uint64_t interval_sum_ns{0};
    uint64_t interval_max_ns{0};
    uint32_t presented_frames{0};
    uint32_t discarded_frames{0};
    uint32_t flags_or{0};     // OR of all 'presented' flags this window
    uint32_t bucket_60hz{0};  // ≤17ms (1 vblank @ 60Hz)
    uint32_t bucket_30hz{0};  // 18–33ms (2 vblanks)
    uint32_t bucket_20hz{0};  // 34–50ms (3 vblanks)
    uint32_t bucket_slow{0};  // 51–100ms (4–6 vblanks)
    uint32_t bucket_idle{0};  // >100ms (treated as paused / load stall)
  };
  FrameProfile profile_{};

  // Cumulative bucket counts across the entire session (IVI_WL_PROFILE=1).
  // Logged once at backend destruction so the user gets a session summary
  // without having to sum the per-window windows.
  FrameProfile session_totals_{};

  // Mirrors DrmBackend::VsyncTrampoline. Flutter calls this with the
  // FlutterDesktopEngineState* as user_data plus an opaque baton; we
  // recover the backend instance and forward to SetVsyncBaton.
  static void VsyncTrampoline(void* user_data, intptr_t baton);

  // Stash the baton; if no feedback is in flight (idle pipeline), kick
  // the baton ourselves via PostOnVsync — otherwise Flutter sits forever
  // waiting for OnVsync from a commit that never happens.
  void SetVsyncBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine, intptr_t baton);

  // Marshal FlutterEngineOnVsync onto the platform task runner's strand.
  // Flutter rejects OnVsync from any other thread.
  void PostOnVsync(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                   intptr_t baton) const;

  // Per-commit feedback request — called from PresentLayers /
  // BlitBackingStoreToWindow / the renderer-config present callbacks
  // BEFORE eglSwapBuffers (which is what mints the wl_surface.commit
  // the feedback binds to). No-op when wp_presentation isn't usable.
  void RequestPresentationFeedback();

  // wp_presentation_feedback listener entry points.
  static void on_feedback_sync_output(void* data,
                                      struct wp_presentation_feedback* fb,
                                      struct wl_output* output);
  static void on_feedback_presented(void* data,
                                    struct wp_presentation_feedback* fb,
                                    uint32_t tv_sec_hi,
                                    uint32_t tv_sec_lo,
                                    uint32_t tv_nano_sec,
                                    uint32_t refresh,
                                    uint32_t seq_hi,
                                    uint32_t seq_lo,
                                    uint32_t flags);
  static void on_feedback_discarded(void* data,
                                    struct wp_presentation_feedback* fb);

  static const struct wp_presentation_feedback_listener feedback_listener_;
};
