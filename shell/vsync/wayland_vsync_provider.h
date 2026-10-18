/*
 * Copyright 2020-2026 Toyota Connected North America
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

#ifndef SHELL_VSYNC_WAYLAND_VSYNC_PROVIDER_H_
#define SHELL_VSYNC_WAYLAND_VSYNC_PROVIDER_H_

#include <atomic>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include <shell/platform/embedder/embedder.h>

#include "presentation_time_client.hpp"  // generated (presentation_time::client)

#include "profiling/frame_profile.h"
#include "view/presentation.h"
#include "vsync/ivsync_provider.h"

#include <wl/wl_ptr.hpp>

namespace profiling {
class MotionToPhoton;  // fed by OnPresented when IVI_M2P_PROFILE is set
}  // namespace profiling

struct wl_registry;
struct wl_surface;

class Engine;
class TaskRunner;

namespace ivi {

class WaylandVsyncProvider;

/// wp_presentation_feedback handler — one per commit; forwards presented /
/// discarded to the provider (method bodies in the .cc, where the provider is
/// complete). Defined here (not hidden in the .cc) so
/// unique_ptr<WpFeedbackHandler> is destructible in any TU that owns a provider
/// by value (e.g. Display).
class WpFeedbackHandler final
    : public presentation_time::client::CWpPresentationFeedback<
          WpFeedbackHandler> {
 public:
  explicit WpFeedbackHandler(WaylandVsyncProvider* owner) : owner_(owner) {}
  ~WpFeedbackHandler();

  void OnSyncOutput(wl_proxy* /*output*/) override {}
  void OnPresented(uint32_t tv_sec_hi,
                   uint32_t tv_sec_lo,
                   uint32_t tv_nsec,
                   uint32_t refresh,
                   uint32_t seq_hi,
                   uint32_t seq_lo,
                   uint32_t flags) override;
  void OnDiscarded() override;

  // frame_start_time (input cutoff) of the frame this feedback was minted for,
  // stamped at RequestFeedback and read at OnPresented (mint happens-before
  // present, so no synchronization needed). 0 = unknown → no frame-accurate
  // drain for this frame.
  uint64_t cutoff_ns_{};
  // The serial RequestFeedback returned for this commit, which the presented
  // event reports to the presentation tracker. Same happens-before as
  // cutoff_ns_.
  uint64_t serial_{};

 private:
  WaylandVsyncProvider* owner_;
};

/**
 * @brief Owns wp_presentation and turns its per-commit
 * wp_presentation_feedback.presented events into vblank-locked
 * FlutterEngineOnVsync callbacks. Replaces the wp_presentation state that used
 * to live in Display plus the duplicated feedback/baton machinery in
 * WaylandEglBackend / WaylandVulkanBackend.
 *
 * Ownership / threading (preserved from the previous in-backend design):
 *   - Display binds wp_presentation during registry enumeration
 *     (TryBindGlobal) and owns the provider; the active backend gets a pointer.
 *   - OnPresented / OnDiscarded fire on Display's event thread; the parked
 *     baton is handed back via PostOnVsync, which marshals
 *     FlutterEngineOnVsync onto the platform task runner (Flutter rejects
 *     OnVsync from any other thread).
 *   - In-flight feedback objects are destroyed race-safely: the firing handler
 *     and Stop() (main thread) coordinate through feedback_mu_.
 *
 * OSGi forward-compat: SetEngine currently tracks a single engine; for the
 * VsyncCoordinator (one source → many bundle engines) this becomes a
 * register/unregister set. The source (wp_presentation) and baton fan-out are
 * deliberately separable here so that extension is additive.
 */
class WaylandVsyncProvider : public IVsyncProvider {
 public:
  WaylandVsyncProvider() = default;
  ~WaylandVsyncProvider() override;

  WaylandVsyncProvider(const WaylandVsyncProvider&) = delete;
  WaylandVsyncProvider& operator=(const WaylandVsyncProvider&) = delete;

  /// Bind wp_presentation if @p interface names it. Called from Display's
  /// registry loop alongside the shells. Returns true if consumed.
  [[nodiscard]] bool TryBindGlobal(wl_registry* registry,
                                   uint32_t name,
                                   std::string_view interface,
                                   uint32_t version);

  /// True once wp_presentation is bound AND its announced clock is usable
  /// (CLOCK_MONOTONIC). When false the backend leaves vsync_callback unset and
  /// Flutter falls back to its wall-clock scheduler.
  [[nodiscard]] bool Usable() const;

  /// Announced presentation clock domain (defaults CLOCK_MONOTONIC until the
  /// clock_id event fires).
  [[nodiscard]] clockid_t ClockId() const { return clock_id_; }

  // --- Backend wiring ------------------------------------------------------
  // SetEngine(engine, runner) + SubmitBaton(engine, baton) are inherited from
  // IVsyncProvider (the shared park/drain/marshal machinery).

  /// The wl_surface per-commit feedback is requested against.
  void SetSurface(wl_surface* surface) { surface_ = surface; }

  /// Mint a wp_presentation_feedback for the current surface — call BEFORE the
  /// wl_surface.commit (eglSwapBuffers). No-op when !Usable() or surface unset.
  /// Returns the serial the commit's presented event will report to the
  /// presentation tracker, or 0 when no feedback was requested.
  uint64_t RequestFeedback();

  /// Where presented events are reported, by the serial RequestFeedback
  /// returned; null detaches. The owner detaches before it goes away.
  void SetPresentationTracker(std::shared_ptr<PresentationTracker> tracker) {
    const std::lock_guard<std::mutex> lock(tracker_mu_);
    tracker_ = std::move(tracker);
  }

  /// Cancel + drain all in-flight feedback, then the base teardown. Idempotent.
  /// Called from FlutterView::~FlutterView before the engine destructs.
  void Stop() override;

  // --- called by WpFeedbackHandler (event thread) --------------------------

  /// presented: record refresh period, hand the baton back via PostOnVsync.
  void OnPresented(uint64_t present_ns,
                   uint32_t refresh_ns,
                   uint64_t msc,
                   uint32_t flags,
                   WpFeedbackHandler* fb);
  /// discarded: still hand the baton back so Flutter keeps scheduling.
  void OnDiscarded(WpFeedbackHandler* fb);

  /// Optional motion-to-photon profiler. Set by Display when IVI_M2P_PROFILE
  /// is enabled; OnPresented feeds it each scanout timestamp (event thread).
  void SetMotionToPhoton(profiling::MotionToPhoton* m2p) { m2p_ = m2p; }

  // Public read of the display refresh period in nanoseconds (0 until the first
  // presented event fixes it). For the debug HUD's frame-rate threshold; the
  // scheduler path uses the protected PeriodNs() override.
  [[nodiscard]] uint32_t RefreshPeriodNs() const {
    return last_refresh_ns_.load(std::memory_order_acquire);
  }

  /// Pace frames on the vblanks the presentations reveal, not on each commit's
  /// presentation. On by default; IVI_WL_VSYNC_GRID=0 turns it off.
  ///
  /// Waiting for a commit to be presented before starting the next frame ties
  /// the frame rate to the host's latency: one commit shown a refresh late
  /// starts the next frame a refresh late, the host's repaint loop idles in
  /// the gap and restarts a refresh late again, and the window settles at
  /// half rate. On the grid, a frame starts on each vblank (at most one per
  /// refresh, from the last presentation and the period) while up to
  /// kGridInFlight commits wait to be shown, so a late commit costs one
  /// refresh, not every one after it.
  void SetGridPacing(bool grid);
  [[nodiscard]] static bool GridPacingEnabledByEnv();

  /// The compositor took platform views' newest frames at @p take_ns (the
  /// rasterizer thread, as it samples them into the window's frame). Feeds
  /// LatchLeadNs(). Calls within a quarter refresh of the last one belong to
  /// the same frame and are ignored.
  void NoteLatch(uint64_t take_ns);
  void NoteLatch() { NoteLatch(NowNs()); }

  /// How long before a vblank the window samples platform views' frames:
  /// the refresh period less the take's phase after the last presentation,
  /// as a decaying maximum so it covers the earliest recent take. 0 until a
  /// take has been seen after a presentation, or with IVI_WL_LATCH_LEAD=0.
  /// Reported to producers as IhsPvCapabilities::latch_lead_ns.
  [[nodiscard]] uint32_t LatchLeadNs() const {
    return latch_lead_ns_.load(std::memory_order_acquire);
  }

  /// Commits that may wait to be shown while grid pacing starts new frames:
  /// kGridInFlight unless IVI_WL_VSYNC_GRID_DEPTH says otherwise. Clamped to
  /// 1..kGridInFlight.
  static constexpr size_t kGridInFlight = 2;
  void SetGridDepth(size_t depth);

  /// On the grid, how long after a refresh's vblank a request, with no frame
  /// started in that refresh, still starts at once (SetPaceStartWindow): 2 ms
  /// unless IVI_WL_VSYNC_START_WINDOW_MS says otherwise (0 to 8, 0 turns it
  /// off; at most half the period counts). A frame started that early still
  /// commits before a host's deadline, so a platform view submitting just
  /// after the vblank is shown a refresh sooner.
  static constexpr uint64_t kStartWindowNs = 2'000'000;
  void SetStartWindow(uint64_t window_ns);

  /// On the grid, how long after each refresh's vblank its frame is handed
  /// out, negative for before it (SetPaceHandOutPhase): 0 unless
  /// IVI_WL_VSYNC_PHASE_MS says otherwise (-8 to 8, fractions allowed). A
  /// host that takes commits right after each vblank shows a frame started on
  /// the vblank two refreshes on; handed out a few milliseconds before the
  /// vblank, its commit lands before the take and it is shown one refresh
  /// sooner. Clamped to half the period.
  static constexpr int64_t kMaxHandOutPhaseNs = 8'000'000;
  void SetHandOutPhase(int64_t phase_ns);

  /// Test seam: the number of commits waiting to be shown, as RequestFeedback
  /// and RetireFeedback keep it, without a compositor.
  void SetInFlightForTest(size_t n) {
    in_flight_.store(n, std::memory_order_release);
    feedback_pending_.store(n != 0, std::memory_order_release);
  }

 protected:
  // IVsyncProvider source hooks (read on the event thread + SubmitBaton).
  // On the grid only a full pipeline holds the baton for a presentation.
  [[nodiscard]] bool IsSourcePending() const override {
    if (grid_.load(std::memory_order_acquire)) {
      return in_flight_.load(std::memory_order_acquire) >=
             grid_depth_.load(std::memory_order_acquire);
    }
    return feedback_pending_.load(std::memory_order_acquire);
  }
  [[nodiscard]] uint32_t PeriodNs() const override {
    const uint32_t r = last_refresh_ns_.load(std::memory_order_acquire);
    return r != 0 ? r : 16'666'667U;  // 60Hz until the first presented event
  }

 private:
  void RetireFeedback(WpFeedbackHandler* fb);  // race-safe destroy

  // wp_presentation handler (OnClockId → clock_id_). Body in the .cc.
  class PresentationHandler final
      : public presentation_time::client::CWpPresentation<PresentationHandler> {
   public:
    explicit PresentationHandler(WaylandVsyncProvider* owner) : owner_(owner) {}
    void OnClockId(uint32_t clk_id) override;

   private:
    WaylandVsyncProvider* owner_;
  };
  std::unique_ptr<PresentationHandler> presentation_;

  wl_surface* surface_{};
  profiling::MotionToPhoton* m2p_{};  // not owned; null unless profiling
  std::atomic<clockid_t> clock_id_{CLOCK_MONOTONIC};
  std::atomic<bool> clock_compatible_{true};

  // Source state read by the IVsyncProvider hooks above; engine/runner/baton
  // now live in the base.
  std::atomic<bool> feedback_pending_{false};
  std::atomic<uint32_t> last_refresh_ns_{0};
  std::atomic<size_t> in_flight_{0};
  // Grid pacing asked for (SetGridPacing), and in force: it starts with the
  // first presentation that gives a refresh period.
  std::atomic<bool> grid_wanted_{false};
  std::atomic<bool> grid_{false};
  void ApplyGrid(bool on);
  std::atomic<size_t> grid_depth_{kGridInFlight};
  std::atomic<uint64_t> start_window_ns_{kStartWindowNs};
  std::atomic<int64_t> hand_out_phase_ns_{0};

  // NoteLatch / LatchLeadNs. The last presentation's time; under latch_mu_,
  // the last take counted and the period the lead was learned at; and the
  // published lead.
  std::atomic<uint64_t> last_present_ns_{0};
  std::mutex latch_mu_;
  uint64_t last_take_ns_{0};
  uint64_t latch_period_ns_{0};
  std::atomic<uint32_t> latch_lead_ns_{0};

  std::mutex feedback_mu_;
  std::vector<std::unique_ptr<WpFeedbackHandler>> feedback_in_flight_;
  std::atomic<uint64_t> feedback_serial_{0};
  std::mutex tracker_mu_;
  std::shared_ptr<PresentationTracker> tracker_;
};

}  // namespace ivi

#endif  // SHELL_VSYNC_WAYLAND_VSYNC_PROVIDER_H_
