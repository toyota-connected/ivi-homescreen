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
  IVsyncProvider() = default;
  virtual ~IVsyncProvider() = default;

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

  /// The frame_start_time of the most recent baton handed to Flutter — i.e. the
  /// input cutoff of the frame currently building/committing. Because a present
  /// in flight keeps the next baton parked, this value is stable across a
  /// single frame's commit, letting a source tag that frame's presentation
  /// feedback with the cutoff of the inputs it actually consumed
  /// (motion-to-photon). 0 until the first baton is delivered.
  [[nodiscard]] uint64_t LastDeliveredFrameStartNs() const {
    return last_frame_start_ns_.load(std::memory_order_acquire);
  }

 private:
  // Marshal one OnVsync onto the runner's strand. No-op when the runner is not
  // wired (the caller must leave the baton parked in that case).
  void PostOnVsync(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                   intptr_t baton,
                   uint64_t now_ns);
  // Drain a parked baton inline (idle pipeline / runner-wired kick latch).
  void DrainParkedBaton();
  // Exchange the parked baton out and marshal it (no profiling — the public
  // DeliverVsync/DeliverDiscard wrappers record the present/discard).
  void DeliverBaton(uint64_t now_ns);

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
