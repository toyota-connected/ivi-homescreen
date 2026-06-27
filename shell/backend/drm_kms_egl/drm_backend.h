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

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "asio/posix/stream_descriptor.hpp"

#include <drm-cxx/core/device.hpp>

#include <shell/platform/embedder/embedder.h>

#include "backend/backend.h"
#include "vsync/ivsync_provider.h"

class DrmCompositor;
class TaskRunner;
namespace homescreen {
class DrmCapture;
class DrmCursor;
class GlCursor;
class ICursorPositionSink;
class DrmSession;
}  // namespace homescreen

// Defined in driver_probe.h — forward-declared here so DrmBackend can hold
// a std::unique_ptr<Resolved> without pulling that header into every TU.
namespace homescreen::driver_probe {
struct Resolved;
}

namespace drm_config {

// Each user-facing knob is tristate: kAuto lets DriverProbe resolve it
// based on cap queries + driver name. Explicit values override the probe.
enum class Compositor : uint8_t { kAuto, kPlanes, kGl };
enum class Modeset : uint8_t { kAuto, kLegacy, kAtomic };
enum class TriState : uint8_t { kAuto, kYes, kNo };
// 32-bit formats + RGB565. Byte order matters for scanout: XRGB vs. XBGR
// differ only in R/B lane ordering, and drivers often advertise just one.
// RGB565 is the fallback for low-end fb drivers (tilcdc, panel-mipi-dbi,
// etc.) whose primary plane doesn't advertise any 32-bit format.
enum class PrimaryFormat : uint8_t {
  kAuto,
  kXrgb8888,
  kXbgr8888,
  kArgb8888,
  kAbgr8888,
  kRgb565,
};

}  // namespace drm_config

struct DrmConfig {
  std::string drm_device;
  // Unset = use the connector's preferred mode as-is (engine + FB at mode
  // size, no framing). Set = use preferred mode for CRTC but size the FB
  // at (width, height) and center it on screen with black borders.
  std::optional<uint32_t> width;
  std::optional<uint32_t> height;
  bool debug_backend{false};

  // Skip HW cursor creation entirely (the global --disable-cursor /
  // disable_cursor config). Pointer events still reach Flutter; there is
  // just no DRM cursor sprite. Equivalent to the IVI_DRM_CURSOR=0 env.
  bool disable_cursor{false};

  // XCursor theme name for the HW cursor sprite (global.cursor_theme). Empty =
  // drm-cxx Theme::discover()'s default ($XCURSOR_THEME, then "default"). The
  // Wayland backend already consumes cursor_theme; this carries it to the DRM
  // hardware cursor so the DRM path honors the same config instead of
  // auto-discovering whatever theme happens to be first on the search path.
  std::string cursor_theme{};

  // Stage the HW cursor into the compositor's atomic commit instead of
  // letting it self-commit (DrmCursor staged mode). kAuto (the default)
  // resolves per driver in DrmBackend::Create: enabled only on nvidia-drm,
  // where a separate cursor commit on the CRTC starves the compositor's
  // PAGE_FLIP_EVENT (content freezes on pointer motion). Everywhere else the
  // self-committing cursor is used — staging it instead couples cursor motion
  // to Flutter frame production, so the cursor stalls whenever the UI is idle.
  // kYes forces staging, kNo forces self-commit. No effect without a
  // compositor.
  drm_config::TriState stage_cursor{drm_config::TriState::kAuto};

  // Unset = pick the highest-ranking connected connector (internal panels
  // preferred, then cable-out). Set = pick the connector whose name
  // (e.g. "eDP-1", "HDMI-A-1") matches; init fails if no such connector
  // is connected. Name format matches --drm-list-modes output.
  std::optional<std::string> connector_name{};

  // Unset = pick the connector's preferred mode (DRM_MODE_TYPE_PREFERRED).
  // Set = pick the first mode matching the spec "<W>x<H>@<R>" (e.g.
  // "1920x1080@120"). Refresh is matched against drmModeModeInfo::vrefresh
  // (integer Hz). Init fails if no matching mode is found.
  std::optional<std::string> mode_spec{};

  // User-facing knobs. All default to kAuto; DriverProbe resolves them
  // into the concrete values stored on DrmBackend::resolved_. See
  // driver_probe.h for the resolution rules.
  drm_config::Compositor compositor{drm_config::Compositor::kAuto};
  drm_config::Modeset modeset{drm_config::Modeset::kAuto};
  drm_config::TriState allow_nonblock_modeset{drm_config::TriState::kAuto};
  drm_config::PrimaryFormat primary_format{drm_config::PrimaryFormat::kAuto};
  drm_config::TriState overlay_planes{drm_config::TriState::kAuto};
  drm_config::TriState explicit_sync{drm_config::TriState::kAuto};
  drm_config::TriState async_flip{drm_config::TriState::kAuto};

  // --drm-no-seat: the session is forced null (see DrmDisplay) so InitDrm
  // takes the direct-open path. This additionally suppresses the
  // foreground-VT guard there, since the operator has explicitly opted into
  // driving the device with no seat/VT arbitration (headless/SSH/kiosk).
  // drmSetMaster is still required and still enforced.
  bool no_seat{false};
};

// PrintDrmModes / ConnectorTypeName moved to display/drm_mode_list.h so the
// --drm-list-modes path is shared verbatim with the drm_kms_vulkan backend.

class DrmBackend : public Shell {
  friend class DrmCompositor;

 public:
  // `session` may be null — when libseat isn't available we fall back to
  // opening the DRM device directly and keeping the legacy foreground-VT
  // check and drmSetMaster call. When non-null, the seat provider owns
  // master handoff on VT switch so both are skipped. The caller retains
  // ownership of the session; it must
  // outlive the backend.
  static std::unique_ptr<DrmBackend> Create(const DrmConfig& cfg,
                                            homescreen::DrmSession* session);
  ~DrmBackend() override;

  DrmBackend(const DrmBackend&) = delete;
  DrmBackend& operator=(const DrmBackend&) = delete;

  void Resize(size_t index, Engine* engine, int32_t w, int32_t h) override;
  void CreateSurface(size_t, struct wl_surface*, int32_t, int32_t) override {}
  bool TextureMakeCurrent() override;
  bool TextureClearCurrent() override;
  FlutterRendererConfig GetRenderConfig() override;
  FlutterCompositor GetCompositorConfig() override;
  bool GetEglContext(BackendEglContext* out) const override;
  // vblank-locked OnVsync via DRM PAGE_FLIP_EVENT. Returns nullptr (so
  // Flutter falls back to its wall-clock scheduler) when the user opts
  // out via IVI_DRM_VSYNC=0 — useful for diagnosing pacing regressions
  // without rebuilding. The trampoline marshals the baton back into
  // SetVsyncBaton, which posts OnVsync onto the platform task runner
  // (Flutter rejects OnVsync from any other thread with
  // kInternalInconsistency).
  VsyncCallback GetVsyncCallback() const override;

#if BUILD_COMPOSITOR
  void RegisterCompositorSurface(
      FlutterPlatformViewIdentifier id,
      std::shared_ptr<ICompositorSurface> surface) override;
  void UnregisterCompositorSurface(FlutterPlatformViewIdentifier id) override;
  void ResizeCompositorSurface(FlutterPlatformViewIdentifier id,
                               int32_t width,
                               int32_t height) override;
#endif

  bool MakeCurrent() const;
  bool ClearCurrent() const;
  bool MakeResourceCurrent() const;
  bool Present();

  // Park the engine's vsync baton (from VsyncTrampoline) into the shared
  // provider, which returns it when the next page flip completes or — for an
  // idle pipeline — drains it inline. Also caches the engine handle for the
  // session-resume path below.
  void SetVsyncBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine, intptr_t baton);

  // Set the running engine handle so OnSessionResumed can call
  // FlutterEngineScheduleFrame. Wired by FlutterView after Engine::Run
  // succeeds. Stored atomically because reads happen on the libseat
  // dispatch thread.
  void SetEngineHandle(FLUTTER_API_SYMBOL(FlutterEngine) engine) override {
    engine_handle_.store(engine, std::memory_order_release);
    // Re-pass engine+runner to the provider. If the runner isn't wired yet
    // this stores a null runner; SetPlatformTaskRunner re-pumps SetEngine
    // once it is, and the provider's #210 kick latch drains any parked baton.
    vsync_.SetEngine(engine,
                     platform_task_runner_.load(std::memory_order_acquire));
  }

  // Hand over the platform task runner so PostOnVsync can marshal back
  // to the FlutterEngineRun thread. Wired by FlutterView from the same
  // post-Engine::Run hook that installs the engine handle. Also starts
  // the asio-based flip monitor on the runner's io_context — without
  // that the drm fd is only polled from WaitForPendingFlip on the
  // rasterizer thread, which deadlocks with vsync_callback because the
  // next Present never starts (it's waiting on OnVsync, which is
  // waiting on PAGE_FLIP_EVENT to be drained).
  void SetPlatformTaskRunner(TaskRunner* runner) override;

  // Cancel the pending async_wait on flip_descriptor_ and detach the fd.
  // MUST be called from FlutterView::~FlutterView before m_flutter_engine
  // destructs — otherwise TaskRunner::~TaskRunner blocks forever joining
  // its io_context worker thread (the async_wait counts as outstanding
  // asio work, so run_one() never returns even after work_.reset()).
  void StopVsyncMonitor() override;

  // Session lifecycle hooks, called from DrmSession's dispatch thread by
  // the libseat trampoline. OnSessionPaused gates the compositor's
  // Present paths and lets the rasterizer drop any flip it had pending;
  // OnSessionResumed clears the pause flag, marks the compositor for a
  // re-modeset on the next commit, and asks the engine to schedule a
  // frame so Flutter actually produces one — without that kick an idle
  // UI never calls Present again and the screen stays blank.
  void OnSessionPaused();
  void OnSessionResumed(int new_fd);

  [[nodiscard]] const drm::Device& device() const { return *drm_dev_; }
  [[nodiscard]] int drm_fd() const { return drm_dev_->fd(); }
  [[nodiscard]] uint32_t connector_id() const { return connector_id_; }
  [[nodiscard]] uint32_t crtc_id() const { return crtc_id_; }
  [[nodiscard]] uint32_t crtc_index() const { return crtc_index_; }
  // Framebuffer size — what the Flutter engine renders at. When the user
  // didn't request a size, this equals the CRTC mode (full-screen). When
  // they did, it's the requested size and the primary plane is centered
  // on the CRTC with black borders.
  [[nodiscard]] uint32_t width() const { return fb_w_; }
  [[nodiscard]] uint32_t height() const { return fb_h_; }
  // CRTC mode size — what the display is actually driven at.
  [[nodiscard]] uint32_t mode_width() const { return mode_.hdisplay; }
  [[nodiscard]] uint32_t mode_height() const { return mode_.vdisplay; }
  [[nodiscard]] uint32_t vrefresh() const { return mode_.vrefresh; }
  [[nodiscard]] const drmModeModeInfo& mode() const { return mode_; }
  [[nodiscard]] EGLDisplay egl_display() const { return egl_display_; }
  [[nodiscard]] gbm_device* gbm() const { return gbm_device_; }

  // Resolved probe output — post-probe, every field is concrete (no kAuto).
  // Forward-declared to avoid a dependency cycle with driver_probe.h;
  // callers that need to read fields include driver_probe.h themselves.
  [[nodiscard]] const homescreen::driver_probe::Resolved& resolved() const {
    return *resolved_;
  }

 private:
  DrmBackend(const DrmConfig& cfg, homescreen::DrmSession* session);
  bool InitDrm();
  bool InitGbm();
  bool InitEgl();
  bool SetInitialMode();
  uint32_t AddFb(gbm_bo* bo) const;
  bool WaitForPendingFlip() const;
  // Drain any readable PAGE_FLIP_EVENT on the drm fd (poll(0)+drmHandleEvent),
  // serialized by drm_event_mutex_ so the rasterizer thread and the asio flip
  // monitor never read the fd concurrently. The poll+read run under the lock so
  // the read can't block: only one thread consumes a given event. Used by both
  // the asio monitor and (when IVI_DRM_RASTER_DRAIN=1) WaitForPendingFlip.
  void DrainFlipEvents() const;
  // True unless IVI_DRM_RASTER_DRAIN=0 — the single source of truth for the
  // raster-thread drain decision, shared by WaitForPendingFlip (both backend
  // and compositor) and the nvidia-drm cursor-staging heuristic. Cached on
  // first call.
  [[nodiscard]] static bool RasterDrainEnabled();
  // Unified PAGE_FLIP_EVENT dispatcher. Registered as the
  // drmEventContext.page_flip_handler from the asio flip monitor; the
  // user_data is always a DrmBackend* (compositor commits also pass
  // `backend_` instead of `this`, so the dispatcher can branch on
  // compositor_->planes_active() to route the per-frame state work to
  // the right place). Returning a pending vsync baton happens here
  // too — we're already on the platform task runner thread, so
  // OnVsync can be called directly without re-posting.
  static void UnifiedPageFlipHandler(int fd,
                                     unsigned int sequence,
                                     unsigned int tv_sec,
                                     unsigned int tv_usec,
                                     void* user_data);
  // Per-flip-complete work for the legacy (non-compositor) path:
  // promote pending→current, drmModeRmFB the previous scanout, clear
  // flip_pending_, record frame stats. Called by UnifiedPageFlipHandler
  // when planes are not active.
  void OnLegacyFlipComplete();
  // FlutterProjectArgs.vsync_callback trampoline. Resolves the
  // FlutterDesktopEngineState user_data back to this backend + the
  // engine handle, then forwards to SetVsyncBaton.
  static void VsyncTrampoline(void* user_data, intptr_t baton);
  // Bind the drm fd to the platform task runner's io_context and
  // arm the first async_wait for POLLIN. Re-armed inside the
  // completion handler so flip events keep flowing without rasterizer
  // involvement.
  void StartFlipMonitor();
  void ArmFlipRead();

  DrmConfig cfg_;
  homescreen::DrmSession* session_ = nullptr;

  // DRM — drm::Device is RAII (closes fd on destruction), unless
  // constructed via Device::from_fd (libseat-owned fd path).
  std::optional<drm::Device> drm_dev_{};
  bool drm_master_ = false;  // true after a successful drmSetMaster
  uint32_t connector_id_ = 0;
  uint32_t crtc_id_ = 0;
  uint32_t crtc_index_ = 0;
  drmModeModeInfo mode_{};
  // Framebuffer dimensions. Equal to mode_ dimensions when cfg_.width/
  // cfg_.height are unset; equal to cfg_ values when set (framed mode).
  // Populated in InitDrm after mode selection.
  uint32_t fb_w_ = 0;
  uint32_t fb_h_ = 0;
  drmModeCrtc* saved_crtc_ = nullptr;

  // Populated by DriverProbe::Resolve() inside Create(). Non-null for the
  // lifetime of the backend. unique_ptr so driver_probe.h isn't needed in
  // this header.
  std::unique_ptr<homescreen::driver_probe::Resolved> resolved_{};

  // GBM
  gbm_device* gbm_device_ = nullptr;
  gbm_surface* gbm_surface_ = nullptr;
  gbm_bo* current_bo_ = nullptr;
  uint32_t current_fb_ = 0;
  gbm_bo* pending_bo_ = nullptr;
  uint32_t pending_fb_ = 0;
  // Atomic so the asio flip monitor (writes false on completion via
  // UnifiedPageFlipHandler → OnLegacyFlipComplete) and the rasterizer
  // thread (reads in WaitForPendingFlip; writes true when queuing the
  // next flip in Present) don't race. Same justification on the
  // compositor's mirror.
  std::atomic<bool> flip_pending_{false};

  // Serializes drmHandleEvent() between the asio flip monitor (platform task
  // runner thread) and WaitForPendingFlip()'s opt-in raster-thread drain, so
  // the two never read the drm fd concurrently. Mutable: DrainFlipEvents() is
  // const (called from the const WaitForPendingFlip). INVARIANT: nothing
  // reachable from UnifiedPageFlipHandler may take this lock — the handler runs
  // under it (inside drmHandleEvent), so re-locking would self-deadlock.
  mutable std::mutex drm_event_mutex_;

  // Set by OnSessionPaused / cleared by OnSessionResumed (libseat VT switch).
  // While paused, scanout is revoked and page flips never complete, so GBM
  // buffers never free — Present() must skip eglSwapBuffers or it blocks
  // forever in gbm_surface_get_free_buffer, wedging the rasterizer thread and
  // hanging teardown. Mirrors DrmCompositor's own paused_ gate.
  std::atomic<bool> session_paused_{false};

  // EGL
  EGLDisplay egl_display_ = EGL_NO_DISPLAY;
  EGLConfig egl_config_ = nullptr;
  EGLContext egl_context_ = EGL_NO_CONTEXT;
  EGLContext egl_resource_context_ = EGL_NO_CONTEXT;
  EGLContext egl_texture_context_ = EGL_NO_CONTEXT;
  EGLSurface egl_surface_ = EGL_NO_SURFACE;

  bool mode_set_ = false;

  // Shared baton park/drain/marshal (#10 vsync fold). DRM's two source-state
  // inputs are dynamic — the legacy flip latch vs. the active compositor's flip
  // latch for IsSourcePending(), and mode_.vrefresh for the refresh period — so
  // we subclass and override the two hooks to read live backend state rather
  // than driving the base's SetSourcePending/SetPeriodNs setters. A nested
  // class so the overrides can reach DrmBackend's private members directly; the
  // bodies live in the .cc where DrmCompositor is a complete type.
  class DrmVsyncProvider final : public ivi::IVsyncProvider {
   public:
    explicit DrmVsyncProvider(DrmBackend* backend) : backend_(backend) {}

   protected:
    [[nodiscard]] bool IsSourcePending() const override;
    [[nodiscard]] uint32_t PeriodNs() const override;

   private:
    DrmBackend* backend_;
  };
  DrmVsyncProvider vsync_{this};

  // FlutterEngine handle. Installed by FlutterView via SetEngineHandle
  // after Engine::Run; also updated defensively by SetVsyncBaton so the
  // vsync_callback path stays consistent. Read by OnSessionResumed and
  // by the PageFlipHandlers when returning vsync batons.
  std::atomic<FLUTTER_API_SYMBOL(FlutterEngine)> engine_handle_{nullptr};
  // Platform task runner used by PostOnVsync to marshal OnVsync calls
  // back to the FlutterEngineRun thread. nullptr until FlutterView
  // wires it after Engine::Run; PostOnVsync no-ops in that window.
  std::atomic<TaskRunner*> platform_task_runner_{nullptr};
  // asio-managed drm fd reader. Bound to the platform task runner's
  // io_context in SetPlatformTaskRunner; async_wait(POLLIN) re-arms
  // itself after every drmHandleEvent so PAGE_FLIP_EVENTs are drained
  // independently of the rasterizer's Present cadence.
  std::optional<asio::posix::stream_descriptor> flip_descriptor_;

  // Frame stats — only active when cfg_.debug_backend is set. Accessed
  // from the rasterizer thread only (Present / PageFlipHandler), so no
  // atomics.
  uint64_t flip_submit_ns_{0};
  uint32_t frame_count_{0};
  uint64_t fps_epoch_ns_{0};
  // Per-second FPS line, active only under cfg_.debug_backend. The unified
  // cadence profile (IVI_VSYNC_PROFILE, "[DrmVsync]") now lives in vsync_,
  // recorded on every page flip via DeliverVsync.
  void RecordFlipComplete();

#if BUILD_COMPOSITOR
  std::unique_ptr<DrmCompositor> compositor_{};
#endif
#if HAVE_DRM_CURSOR
  std::unique_ptr<homescreen::DrmCursor> cursor_{};
#endif
  // Composited cursor fallback, created only when no HW cursor plane is
  // available (this display controller has none) and the cursor isn't
  // disabled. Drawn into FBO 0 each Present.
  std::unique_ptr<homescreen::GlCursor> gl_cursor_{};
#if HAVE_DRM_CAPTURE
  std::unique_ptr<homescreen::DrmCapture> capture_;
#endif

 public:
  [[nodiscard]] homescreen::DrmCursor* drm_cursor() const {
#if HAVE_DRM_CURSOR
    return cursor_.get();
#else
    return nullptr;
#endif
  }

  // The composited cursor sink (nullptr when a HW cursor is in use or the
  // cursor is disabled). Defined out-of-line where GlCursor is complete.
  [[nodiscard]] homescreen::ICursorPositionSink* gl_cursor() const;

  // Called once per frame from both Present() (GL fallback) and
  // DrmCompositor::PresentLayers (plane path). Always defined so call
  // sites stay unconditional; when HAVE_DRM_CAPTURE is off (no Blend2D)
  // the body compiles to nothing.
  void MaybeCaptureSnapshot();
};
