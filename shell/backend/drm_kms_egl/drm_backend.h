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

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "asio/posix/stream_descriptor.hpp"

#include <drm-cxx/core/device.hpp>

#include <shell/platform/embedder/embedder.h>

#include "backend/backend.h"
#include "backend/drm_kms_egl/drm_output_context.h"
#include "backend/drm_kms_egl/flip_sink.h"
#include "vsync/ivsync_provider.h"

class DrmCompositor;
class DrmDisplay;
class TaskRunner;
namespace ihs::hud {
class GlHud;
}  // namespace ihs::hud

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

  // wayland-leased-drm: the leased connector's DRM object id. Takes precedence
  // over connector_name and the rank heuristic -- on a lease the compositor
  // decides which connector we get, so neither an operator's name pin nor a
  // "prefer internal panels" ranking has standing. A lease may contain more
  // than one connector (the requested set is only a suggestion), so without
  // this the rank heuristic can drive the wrong panel and leave the requested
  // one dark. 0 = not leased; use the rules above.
  uint32_t lease_connector_id{0};

  // wayland-leased-drm: polled before each commit. A revoked lease's KMS
  // objects are gone from the fd's view, so every commit naming them fails and
  // produces no PAGE_FLIP_EVENT. Empty = never gated (the path-opened tiers,
  // which have no lease to lose).
  std::function<bool()> lease_revoked{};

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

class DrmBackend : public Backend, public IFlipSink {
  friend class DrmCompositor;

 public:
  // `session` may be null — when libseat isn't available we fall back to
  // opening the DRM device directly and keeping the legacy foreground-VT
  // check and drmSetMaster call. When non-null, the seat provider owns
  // master handoff on VT switch so both are skipped. The caller retains
  // ownership of the session; it must
  // outlive the backend.
  // @p shared_device is the card opened + held at master by the device-context
  // (DrmDisplay::SharedDevice); the backend scans out through it without taking
  // master itself. It is non-owning and must outlive the backend.
  // @p display is the device-context; the compositor uses it to reserve/exclude
  // planes so co-tenant views on the same card don't collide on a shared
  // overlay. Non-owning; outlives the backend.
  static std::unique_ptr<DrmBackend> Create(const DrmConfig& cfg,
                                            homescreen::DrmSession* session,
                                            drm::Device* shared_device,
                                            DrmDisplay* display);

  // The device-context this backend scans out through (plane reservation).
  [[nodiscard]] DrmDisplay* display() const { return drm_display_; }
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

  // bind a view id to the compositor that scans it out to its output
  // (a sibling backend's CRTC on the same card). Called by App when it attaches
  // additional views to the one engine whose compositor callbacks live on this
  // (primary) backend. No-op when BUILD_COMPOSITOR is off.
  void RegisterViewOutput(int64_t view_id, DrmCompositor* compositor);

  // resolve a named connector to a distinct (unused) CRTC on this card,
  // build a compositor for it that shares this backend's EGL/GBM, and bind it
  // to view_id (which becomes an added Flutter view). Returns false if the
  // connector isn't connected or no free CRTC remains. No-op / false when
  // BUILD_COMPOSITOR is off.
  bool AddOutput(const std::string& connector_name, int64_t view_id);

  // Geometry FlutterView needs to FlutterEngineAddView each additional output.
  struct ViewOutputSpec {
    int64_t view_id;
    uint32_t width;
    uint32_t height;
  };
  [[nodiscard]] std::vector<ViewOutputSpec> AdditionalViews() const;
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
  void SetVsyncParked(const bool parked) override { vsync_.SetParked(parked); }

  void StopVsyncMonitor() override;

  // Unified PAGE_FLIP_EVENT dispatcher (drmModeEventContext.page_flip_handler
  // signature). The card's flip-reader thread (owned by DrmDisplay) registers
  // this as the handler; user_data is always a DrmBackend*, so a single reader
  // on the shared fd routes each flip to the backend that committed it. Clears
  // that backend's flip_pending_ and returns its vsync baton (DeliverVsync
  // marshals OnVsync onto the platform runner, so running on the reader thread
  // is safe). Public so DrmDisplay can register it without depending on this
  // (EGL-only) type.
  static void UnifiedPageFlipHandler(int fd,
                                     unsigned int sequence,
                                     unsigned int tv_sec,
                                     unsigned int tv_usec,
                                     void* user_data);

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
  // The connector this backend modeset, by name. Empty until a connector is
  // picked, which is why it is optional rather than a bare string.
  [[nodiscard]] std::optional<std::string> BoundOutputName() const override {
    return bound_connector_name_;
  }
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
  DrmBackend(DrmConfig cfg, homescreen::DrmSession* session);
  bool InitDrm();
  bool InitGbm();
  bool InitEgl();
  bool SetInitialMode();
  uint32_t AddFb(gbm_bo* bo) const;
  bool WaitForPendingFlip() const;
  // True unless IVI_DRM_RASTER_DRAIN=0 — the single source of truth for the
  // raster-thread drain decision, shared by WaitForPendingFlip (both backend
  // and compositor) and the nvidia-drm cursor-staging heuristic. Cached on
  // first call.
  [[nodiscard]] static bool RasterDrainEnabled();
  // Per-flip-complete work for the legacy (non-compositor) path:
  // promote pending→current, drmModeRmFB the previous scanout, clear
  // flip_pending_, record frame stats. Called by UnifiedPageFlipHandler
  // when planes are not active.
  void OnLegacyFlipComplete();
  // IFlipSink: this backend armed a legacy Present() page flip and registered
  // itself as the flip's user_data. Routes to the legacy completion.
  void OnFlipEvent(unsigned int tv_sec, unsigned int tv_usec) override;
  // Return the vsync baton stamped with a flip's scanout time. Called by the
  // vsync-driving output (this backend for legacy, or its primary compositor)
  // from a PAGE_FLIP_EVENT. Secondary outputs do not call this — one
  // engine has one vsync source.
  void DeliverVsyncFromFlip(unsigned int tv_sec, unsigned int tv_usec);
  // FlutterProjectArgs.vsync_callback trampoline. Resolves the
  // FlutterDesktopEngineState user_data back to this backend + the
  // engine handle, then forwards to SetVsyncBaton.
  static void VsyncTrampoline(void* user_data, intptr_t baton);

  DrmConfig cfg_;
  homescreen::DrmSession* session_ = nullptr;
  // Device-context (non-owning). Used for cross-view plane reservation on the
  // shared card.
  DrmDisplay* drm_display_ = nullptr;

  // The shared card (opened + held at master by DrmDisplay::SharedDevice).
  // Non-owning: the device-context owns it and outlives this backend, so the
  // backend neither takes master nor closes the fd.
  drm::Device* drm_dev_ = nullptr;
  uint32_t connector_id_ = 0;
  std::optional<std::string> bound_connector_name_{};
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

  // Set by OnSessionPaused / cleared by OnSessionResumed (libseat VT switch).
  // While paused, scanout is revoked and page flips never complete, so GBM
  // buffers never free — Present() must skip eglSwapBuffers or it blocks
  // forever in gbm_surface_get_free_buffer, wedging the rasterizer thread and
  // hanging teardown. Mirrors DrmCompositor's own paused_ gate.
  std::atomic<bool> session_paused_{false};

  // One-shot latch so the lease-revocation notice is logged once, not once per
  // frame. The gate itself lives in cfg_.lease_revoked.
  std::atomic<bool> lease_revoked_logged_{false};

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
  // view_id -> the compositor that scans that view out to its output.
  // Seeded {0 -> compositor_} (the implicit view on this backend's CRTC); App
  // registers additional views on sibling backends' compositors via
  // RegisterViewOutput. The present/backing-store callbacks route by view_id.
  std::unordered_map<int64_t, DrmCompositor*> view_outputs_;
  [[nodiscard]] DrmCompositor* CompositorForView(int64_t view_id) const;

  // Additional outputs this backend drives for one engine: each owns a
  // compositor bound to its own CRTC, sharing this backend's EGL/GBM. Built by
  // AddOutput; destroyed with the backend.
  struct ExtraOutput {
    int64_t view_id;
    std::unique_ptr<DrmCompositor> compositor;
    uint32_t width;
    uint32_t height;
  };
  std::vector<ExtraOutput> extra_outputs_;
  // CRTCs already claimed (primary + each AddOutput) so additional outputs pick
  // a distinct one. Seeded with the primary CRTC after InitDrm.
  std::vector<uint32_t> used_crtcs_;
  bool ResolveOutputContext(const std::string& connector_name,
                            DrmOutputContext& out);
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

#if BUILD_HUD
  // Debug HUD (imgui GL) drawn into FBO 0 before the swap on the GL-swap
  // present path. (The plane-compositor path scans out planes directly and is
  // not covered.) Created lazily on first present when IVI_HUD / [hud].enable
  // is set.
  std::unique_ptr<ihs::hud::GlHud> hud_;
  bool hud_enabled_{false};
  bool hud_checked_{false};
  bool hud_init_failed_{false};
  uint64_t hud_last_present_ns_{0};
  std::array<float, 60> hud_interval_ms_{};
  uint32_t hud_interval_head_{0};
  uint32_t hud_interval_count_{0};
  // Lazily create hud_ (env/config gated); returns true if it exists.
  bool EnsureHud();
  // Roll the present-interval window and produce this frame's stats; @dt_s out.
  ihs::hud::HudStats ComputeHudStats(float& dt_s);
  // GL-swap path: draw the HUD into FBO 0 before the buffer swap.
  void MaybeRenderHud(const FlutterLayer** layers, size_t count);

 public:
  // Plane-compositor path: render the HUD into an offscreen RGBA texture and
  // return its GL name (0 when disabled), so DrmCompositor can composite it
  // onto its scanout buffer (with the same y-flip it uses for app layers).
  unsigned int RenderHudTexture(uint32_t width, uint32_t height);

 private:
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
