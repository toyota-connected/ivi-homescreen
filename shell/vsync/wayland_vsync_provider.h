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
  void RequestFeedback();

  /// Cancel + drain all in-flight feedback, then the base teardown. Idempotent.
  /// Called from FlutterView::~FlutterView before the engine destructs.
  void Stop() override;

  // --- called by WpFeedbackHandler (event thread) --------------------------

  /// presented: record refresh period, hand the baton back via PostOnVsync.
  void OnPresented(uint64_t present_ns,
                   uint32_t refresh_ns,
                   WpFeedbackHandler* fb);
  /// discarded: still hand the baton back so Flutter keeps scheduling.
  void OnDiscarded(WpFeedbackHandler* fb);

  /// Optional motion-to-photon profiler. Set by Display when IVI_M2P_PROFILE
  /// is enabled; OnPresented feeds it each scanout timestamp (event thread).
  void SetMotionToPhoton(profiling::MotionToPhoton* m2p) { m2p_ = m2p; }

 protected:
  // IVsyncProvider source hooks (read on the event thread + SubmitBaton).
  [[nodiscard]] bool IsSourcePending() const override {
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

  std::mutex feedback_mu_;
  std::vector<std::unique_ptr<WpFeedbackHandler>> feedback_in_flight_;
};

}  // namespace ivi

#endif  // SHELL_VSYNC_WAYLAND_VSYNC_PROVIDER_H_
