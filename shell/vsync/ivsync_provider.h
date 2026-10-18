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

#ifndef SHELL_VSYNC_IVSYNC_PROVIDER_H_
#define SHELL_VSYNC_IVSYNC_PROVIDER_H_

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <shell/platform/embedder/embedder.h>

#include "profiling/frame_profile.h"

class TaskRunner;

namespace ivi {

/**
 * @brief Backend-spanning vsync baton machinery shared by every presentation
 * source (Wayland wp_presentation, DRM page-flip/vblank, software page-flip).
 *
 * Flutter hands the embedder an opaque `baton` via the vsync_callback and
 * expects exactly one FlutterEngineOnVsync(baton) back, marshaled onto the
 * platform task runner (Flutter rejects OnVsync from any other thread). The
 * park/drain/marshal dance is identical across backends; only three things
 * differ, which subclasses supply:
 *   - IsSourcePending(): is a present already in flight (so the next present
 *     event will return the baton) vs. an idle pipeline (drain inline)?
 *   - PeriodNs(): the display refresh period for frame_target_time.
 *   - the source event itself, which calls DeliverVsync() when a frame is
 *     actually presented.
 *
 * Cold-start safety (#210): a baton parked before the platform task runner is
 * wired is LEFT parked (not exchanged out and dropped); SetRunner() drains it
 * via the kick latch once the runner arrives. Taking the baton exactly once —
 * by whichever of SubmitBaton/SetRunner/DeliverVsync first sees the runner
 * wired — keeps the hand-off race-free.
 */
class IVsyncProvider {
 public:
  IVsyncProvider();
  virtual ~IVsyncProvider();

  IVsyncProvider(const IVsyncProvider&) = delete;
  IVsyncProvider& operator=(const IVsyncProvider&) = delete;

  /// Engine handle + platform runner used to marshal FlutterEngineOnVsync.
  /// Wiring the runner also drains any baton parked during bring-up.
  void SetEngine(FLUTTER_API_SYMBOL(FlutterEngine) engine, TaskRunner* runner);

  /// Park the engine's vsync baton (from the backend's VsyncTrampoline). If a
  /// present is already in flight it will return the baton; an idle pipeline is
  /// drained inline (when the runner is wired) so Flutter is not stalled.
  void SubmitBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine, intptr_t baton);

  /// Drop the parked baton + clear engine/runner. Idempotent. Called at
  /// teardown. An unanswered baton is a documented leak, not a crash.
  /// Sources with extra teardown (e.g. outstanding feedback proxies) override
  /// and call the base.
  virtual void Stop();

  /// The presentation source (page-flip / wp_presentation.presented) calls this
  /// when a frame was actually shown, handing @p frame_start_ns (its real
  /// timestamp) back to Flutter. Public so a composing owner (e.g. a DRM sink
  /// whose page-flip handler is its own member) can drive it.
  void DeliverVsync(uint64_t frame_start_ns);

  // --- Source state (composition path) -------------------------------------
  // A composing owner that does not subclass drives these instead of overriding
  // the hooks below: SetSourcePending() as a present enters/leaves flight, and
  // SetPeriodNs() when the refresh period is known.

  /// Mark whether a present is in flight (gates SubmitBaton's inline drain).
  void SetSourcePending(bool pending) {
    source_pending_.store(pending, std::memory_order_release);
  }
  /// Set the refresh period used for frame_target_time.
  void SetPeriodNs(uint32_t period_ns) {
    period_ns_.store(period_ns, std::memory_order_release);
  }

  /// Enable per-present cadence diagnostics: a window summary every 60 presents
  /// and a session summary on Stop(). @p label tags the log lines (e.g.
  /// "SoftwareVsync"); @p enabled is the resolved gate (e.g.
  /// profiling::FrameProfile::Enabled("IVI_VSYNC_PROFILE")). Call once at
  /// setup.
  void EnableProfile(bool enabled, std::string label);

  /// Like DeliverVsync, but for a frame that was NOT presented (discarded /
  /// superseded / failed flip): counts a discard and hands the baton back on
  /// the wall clock so Flutter keeps scheduling.
  void DeliverDiscard();

  /// Deliver a parked baton on the wall clock if one is parked, returning true
  /// iff it delivered. For control-flow kicks where no source event will fire
  /// to return the baton — a VT resume or a post-modeset drain (drmModeSetCrtc
  /// emits no page-flip event) waking an idle UI. Unlike DeliverDiscard it
  /// records no profile discard: these are not dropped frames.
  bool DeliverParkedBaton();

  /// Park (true) or unpark (false) frame production.
  ///
  /// While parked the baton is simply not handed back: Flutter asked for a
  /// vsync and gets no answer, so it builds no frame and the whole pipeline
  /// -- UI thread, rasterizer, GPU, scanout -- goes quiet. That is what a
  /// view whose display was unplugged should do, and it needs no cooperation
  /// from the source: the baton stays in vsync_baton_ exactly as it already
  /// does during bring-up, and unparking delivers it.
  ///
  /// Deliberately not Stop(): Stop() clears engine and runner and is a
  /// teardown with no way back, whereas this is reversible and keeps the
  /// engine warm.
  ///
  /// Safe to call from any thread; idempotent.
  void SetParked(bool parked);

  /// True while frame production is parked by SetParked.
  [[nodiscard]] bool IsParked() const {
    return parked_.load(std::memory_order_acquire);
  }

  /// Hand Flutter at most one vsync per refresh, on the display's vblanks.
  ///
  /// Without it a request made while nothing is in flight is answered at once,
  /// and nothing is in flight from the moment a baton goes out until that
  /// frame commits: a ticker's next request lands in that window, gets the
  /// next baton at once, and so on -- hundreds of empty frames in the first
  /// milliseconds after a vblank, and a frame started before the one in front
  /// of it has presented, which then waits a whole refresh behind it.
  ///
  /// Paced, a request made after a baton has gone out this refresh waits for
  /// the next vblank (from the last presentation and the period), and gets
  /// that vblank as its frame start. A flip still hands the baton back at
  /// once, and a request after a refresh with no baton is still answered at
  /// once, so an idle display wakes as quickly as before.
  void SetPaceToVblank(bool pace) {
    pace_.store(pace, std::memory_order_release);
  }

  /// Make a presentation obey the pacing too (needs SetPaceToVblank).
  ///
  /// By default a flip hands the baton back at once: on DRM the flip is the
  /// vblank, so it is the start of a new refresh by definition. A source that
  /// lets more than one present be in flight is different: the presentation of
  /// an older frame can arrive just after a baton went out on this refresh's
  /// vblank, and answering it would start a second frame in one refresh. With
  /// this on, such a presentation leaves the baton for the next vblank.
  void SetPaceFlips(bool pace_flips) {
    pace_flips_.store(pace_flips, std::memory_order_release);
  }

  /// Keep frames on the vblank while frames keep coming (needs
  /// SetPaceToVblank).
  ///
  /// By default a request made when no baton went out this refresh is
  /// answered at once, so an idle display wakes quickly. When what asks for
  /// frames is something mid-refresh -- a platform view submitting its own
  /// frames -- that rule starts every frame at the moment of the request, and
  /// the window's frames drift off the vblank. With this on, a request is
  /// answered at once only after an idle refresh with no baton (two periods
  /// since the last hand-out's vblank), or within SetPaceStartWindow of the
  /// vblank; otherwise it waits for the next vblank.
  void SetPaceKeepPhase(bool keep) {
    pace_keep_phase_.store(keep, std::memory_order_release);
  }

  /// With SetPaceKeepPhase: a request this soon after the vblank that started
  /// the refresh, with no baton out in it yet, is answered at once instead of
  /// on the next vblank, with that vblank as its frame start. A platform view
  /// that submits just after the vblank then gets its frame into this
  /// refresh, as a frame started now can still meet the host's deadline. At
  /// most half the period counts, so keep-phase still holds the rest of the
  /// refresh. 0 (the default) keeps every such request for the next vblank.
  void SetPaceStartWindow(uint64_t window_ns) {
    pace_start_window_ns_.store(window_ns, std::memory_order_release);
  }

  /// With SetPaceToVblank: hand frames out this long after the vblank that
  /// begins their refresh (negative: before it), instead of on it. The frame
  /// starts when it is handed out and still targets the next vblank, so a
  /// host that takes commits right after each vblank gets the commit before
  /// its take with a negative phase, and one that takes them late in the
  /// refresh gets a later input cutoff with a positive one. Clamped to half
  /// a period either way. 0 (the default) hands out on the vblank.
  void SetPaceHandOutPhase(int64_t phase_ns) {
    pace_phase_ns_.store(phase_ns, std::memory_order_release);
  }
  [[nodiscard]] int64_t PaceHandOutPhaseNs() const {
    return pace_phase_ns_.load(std::memory_order_acquire);
  }

  /// The DRM backends' default: paced unless IVI_VSYNC_PACE=0.
  [[nodiscard]] static bool PaceEnabledByEnv();

  /// Test seam: the clock pacing reads (CLOCK_MONOTONIC nanoseconds) and what
  /// it does to deliver at a later time (normally a timer on the runner). A
  /// test calls FireDeferredForTest() to run the delivery it was asked for.
  void SetPacingClockForTest(std::function<uint64_t()> now,
                             std::function<void(uint64_t due_ns)> schedule) {
    now_for_test_ = std::move(now);
    schedule_for_test_ = std::move(schedule);
  }
  void FireDeferredForTest() {
    OnDeferredDue(deferred_due_ns_.load(std::memory_order_acquire));
  }

 protected:
  /// True when a present is already in flight, so SubmitBaton should wait for
  /// the source rather than draining inline. Default reads source_pending_;
  /// subclasses with their own pending state (e.g. Wayland feedback) override.
  [[nodiscard]] virtual bool IsSourcePending() const {
    return source_pending_.load(std::memory_order_acquire);
  }

  /// Display refresh period in nanoseconds (for frame_target_time). Default
  /// reads period_ns_; subclasses may override with their own source.
  [[nodiscard]] virtual uint32_t PeriodNs() const {
    return period_ns_.load(std::memory_order_acquire);
  }

  [[nodiscard]] FLUTTER_API_SYMBOL(FlutterEngine) engine() const {
    return engine_.load(std::memory_order_acquire);
  }

  /// CLOCK_MONOTONIC nanoseconds, or the test clock when one is set.
  [[nodiscard]] uint64_t NowNs() const;

 public:
  /// The frame_start_time of the most recent baton handed to Flutter — i.e. the
  /// input cutoff of the frame currently building/committing. When a present
  /// in flight keeps the next baton parked, this value is stable across a
  /// single frame's commit, letting a source tag that frame's presentation
  /// feedback with the cutoff of the inputs it actually consumed
  /// (motion-to-photon). A source that lets the next baton go out while a
  /// commit waits (Wayland grid pacing) makes it approximate. 0 until the
  /// first baton is delivered. Public so a
  /// backend that owns (rather than subclasses) the provider — the DRM/KMS
  /// page-flip path — can read the cutoff when recording the scanout endpoint.
  [[nodiscard]] uint64_t LastDeliveredFrameStartNs() const {
    return last_frame_start_ns_.load(std::memory_order_acquire);
  }

  /// True when a baton is parked awaiting delivery (i.e. the engine has asked
  /// for a vsync that has not yet been handed back). Lets a scheduling owner
  /// (e.g. a consumer-paced source) decide whether there is anything to deliver
  /// before consuming a credit / arming a rate-ceiling timer, without taking
  /// the baton.
  [[nodiscard]] bool HasParkedBaton() const {
    return vsync_baton_.load(std::memory_order_acquire) != 0;
  }

 private:
  // Frame production is parked: DeliverBaton leaves the baton in
  // vsync_baton_ instead of posting it, and SetParked(false) delivers it.
  std::atomic<bool> parked_{false};

  // Marshal one OnVsync onto the runner's strand. No-op when the runner is not
  // wired (the caller must leave the baton parked in that case). @p start_ns
  // is the frame's start and input cutoff, @p target_ns the vblank it is for.
  void PostOnVsync(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                   intptr_t baton,
                   uint64_t start_ns,
                   uint64_t target_ns);
  // Drain a parked baton inline (idle pipeline / runner-wired kick latch).
  // Hands out a parked baton now. @p vblank_ns is the vblank that begins the
  // frame's refresh; 0 means an unpaced frame starting at the current time.
  void DrainParkedBaton(uint64_t vblank_ns = 0);
  // Exchange the parked baton out and marshal it (no profiling — the public
  // DeliverVsync/DeliverDiscard wrappers record the present/discard).
  // @p on_vblank: @p now_ns is the vblank that begins the frame's refresh;
  // the frame starts at that vblank plus the hand-out phase, or now if that
  // is still to come.
  void DeliverBaton(uint64_t now_ns, bool on_vblank);
  // The hand-out phase, clamped to half a period, and when the frame of the
  // refresh beginning at @p vblank_ns is handed out.
  [[nodiscard]] int64_t PhaseNs() const;
  [[nodiscard]] uint64_t HandOutNs(uint64_t vblank_ns) const;

  // Pacing (SetPaceToVblank). The earliest time the next baton may go out,
  // set on every hand-out to the vblank after the refresh it went out in; the
  // time of the last real presentation, whose phase the vblanks are counted
  // from; and the time a deferred delivery is due (0 = none armed).
  [[nodiscard]] uint64_t NextVblankAfter(uint64_t t_ns) const;
  // @p on_vblank: @p at_ns is a vblank, which may be counted from an older
  // presentation than the last.
  void NoteHandOut(uint64_t at_ns, bool on_vblank);
  void ScheduleDeferred(uint64_t due_ns);
  void OnDeferredDue(uint64_t due_ns);
  // With SetPaceFlips: true when a baton already went out this refresh, so a
  // presentation or discard must leave the baton for the next vblank (and
  // arms that delivery if one is parked).
  bool DeferToPace();
  std::atomic<bool> pace_{false};
  std::atomic<bool> pace_flips_{false};
  std::atomic<bool> pace_keep_phase_{false};
  std::atomic<uint64_t> pace_start_window_ns_{0};
  std::atomic<int64_t> pace_phase_ns_{0};
  std::atomic<uint64_t> last_vblank_ns_{0};
  std::atomic<uint64_t> next_allowed_ns_{0};
  std::atomic<uint64_t> deferred_due_ns_{0};
  // Deferred waits run on the runner's io_context and may fire after this
  // provider is stopped or gone; they reach it only through this, under its
  // lock. SetEngine() sets the owner; Stop() and the destructor clear it.
  struct PaceTimer;
  std::shared_ptr<PaceTimer> pace_timer_;
  std::function<uint64_t()> now_for_test_;
  std::function<void(uint64_t)> schedule_for_test_;

  std::atomic<intptr_t> vsync_baton_{0};
  std::atomic<FLUTTER_API_SYMBOL(FlutterEngine)> engine_{nullptr};
  std::atomic<TaskRunner*> runner_{nullptr};

  // frame_start_time of the last baton delivered via PostOnVsync — the input
  // cutoff of the frame that baton drives (see LastDeliveredFrameStartNs).
  std::atomic<uint64_t> last_frame_start_ns_{0};

  // Default source state for the composition path (unused when a subclass
  // overrides IsSourcePending()/PeriodNs()). 60Hz period until told otherwise.
  std::atomic<bool> source_pending_{false};
  std::atomic<uint32_t> period_ns_{16'666'667};

  // Per-present cadence diagnostics (shared by all sources). Records on the
  // event thread (single writer per source) — no lock needed.
  bool profile_enabled_{false};
  std::string profile_label_;
  profiling::FrameProfile profile_;
  // Latches Stop()'s session-summary log to one emission, so a double Stop()
  // (or a Stop() followed by a teardown path that calls it again) doesn't
  // print a duplicate summary line.
  bool summary_logged_{false};
};

}  // namespace ivi

#endif  // SHELL_VSYNC_IVSYNC_PROVIDER_H_
