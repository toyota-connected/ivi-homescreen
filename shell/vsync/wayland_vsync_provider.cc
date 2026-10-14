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

#include "vsync/wayland_vsync_provider.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>

#include <wayland-client.h>

#include <wl/client_helpers.hpp>  // wl::SetupHandler
#include <wl/proxy_impl.hpp>      // wl::construct

#include "asio/post.hpp"

#include "libflutter_engine.h"
#include "logging/logging.h"
#include "profiling/motion_to_photon.h"
#include "task_runner.h"

namespace ivi {

namespace ptc = presentation_time::client;

namespace {
// Outstanding-feedback cap: a misbehaving compositor that never sends
// presented/discarded would otherwise leak wl_proxy ids without bound. Flutter
// pipelines <=4 commits in practice.
constexpr size_t kFeedbackInFlightCap = 16;
// 10Hz floor on the reported refresh period — clamps a hostile/virtual
// compositor from wedging the engine on a far-future frame_target_time.
constexpr uint32_t kMaxPlausibleRefreshNs = 100'000'000;
// LatchLeadNs decays toward later takes by this fraction of the gap per frame:
// about half in 0.7 s at 60 Hz. An earlier take widens it at once.
constexpr uint64_t kLatchLeadDecay = 64;
}  // namespace

// Handler method bodies — the classes are defined in the header so unique_ptr
// members are destructible in any TU; the bodies live here where the provider
// is complete.
void WaylandVsyncProvider::PresentationHandler::OnClockId(
    const uint32_t clk_id) {
  owner_->clock_id_.store(static_cast<clockid_t>(clk_id),
                          std::memory_order_release);
  // FlutterEngineGetCurrentTime is CLOCK_MONOTONIC; only then are the
  // compositor's presented timestamps usable as frame_start_time directly.
  owner_->clock_compatible_.store(clk_id == CLOCK_MONOTONIC,
                                  std::memory_order_release);
  ihs::log::debug("[WaylandVsync] wp_presentation clock_id={}", clk_id);
}

WpFeedbackHandler::~WpFeedbackHandler() {
  if (auto* p = this->GetProxy()) {
    wl_proxy_destroy(p);
  }
}

void WpFeedbackHandler::OnPresented(const uint32_t tv_sec_hi,
                                    const uint32_t tv_sec_lo,
                                    const uint32_t tv_nsec,
                                    const uint32_t refresh,
                                    const uint32_t seq_hi,
                                    const uint32_t seq_lo,
                                    const uint32_t flags) {
  // Reject malformed tv_sec_hi: CLOCK_MONOTONIC seconds-since-boot has never
  // reached 2^32 (~136 years); a non-zero high word would overflow the *1e9
  // below. Fall back to the discarded path (still returns the baton).
  if (tv_sec_hi != 0) {
    owner_->OnDiscarded(this);
    return;
  }
  const uint64_t present_ns =
      static_cast<uint64_t>(tv_sec_lo) * 1'000'000'000ULL + tv_nsec;
  const uint64_t msc = (static_cast<uint64_t>(seq_hi) << 32U) | seq_lo;
  owner_->OnPresented(present_ns, refresh, msc, flags, this);
}

void WpFeedbackHandler::OnDiscarded() {
  owner_->OnDiscarded(this);
}

WaylandVsyncProvider::~WaylandVsyncProvider() {
  Stop();
}

bool WaylandVsyncProvider::TryBindGlobal(wl_registry* registry,
                                         const uint32_t name,
                                         const std::string_view interface,
                                         const uint32_t version) {
  if (interface != ptc::wp_presentation_traits::wl_iface().name) {
    return false;
  }
  // Cap at v2 (zero_copy feedback flag); we use no v2-only request.
  const uint32_t bind_ver =
      std::min(version, std::min(2u, ptc::wp_presentation_traits::version));
  auto* raw = wl_registry_bind(
      registry, name, &ptc::wp_presentation_traits::wl_iface(), bind_ver);
  presentation_ = std::make_unique<PresentationHandler>(this);
  // _SetProxy adopts the proxy AND installs the static event listener
  // (clock_id) with this handler as the listener target.
  presentation_->_SetProxy(reinterpret_cast<wl_proxy*>(raw));
  // Vsync/scanout-cadence profile gate (evaluated once, here on the main thread
  // before the event thread runs). IVI_VSYNC_PROFILE is the shared gate for the
  // backend-spanning vsync layer; the "WaylandVsync" label distinguishes this
  // source from the per-renderer present/swap profiles (IVI_WL_PROFILE = egl
  // swap, IVI_VK_PROFILE = vk present). IVI_PROFILE enables everything.
  EnableProfile(profiling::FrameProfile::Enabled("IVI_VSYNC_PROFILE"),
                "WaylandVsync");
  if (const char* env = std::getenv("IVI_WL_VSYNC_START_WINDOW_MS")) {
    char* end = nullptr;
    const double ms = std::strtod(env, &end);
    if (end == env || *end != '\0' || !(ms >= 0 && ms <= 8)) {
      ihs::log::warn(
          "[WaylandVsync] IVI_WL_VSYNC_START_WINDOW_MS={} ignored (0-8)", env);
    } else {
      start_window_ns_.store(static_cast<uint64_t>(ms * 1e6),
                             std::memory_order_release);
    }
  }
  SetGridPacing(GridPacingEnabledByEnv());
  if (const char* env = std::getenv("IVI_WL_VSYNC_GRID_DEPTH")) {
    const unsigned long depth = std::strtoul(env, nullptr, 10);
    if (depth < 1 || depth > kGridInFlight) {
      ihs::log::warn("[WaylandVsync] IVI_WL_VSYNC_GRID_DEPTH={} ignored (1-{})",
                     env, kGridInFlight);
    } else {
      SetGridDepth(depth);
    }
  }
  ihs::log::debug("[WaylandVsync] bound wp_presentation v{}", bind_ver);
  return true;
}

bool WaylandVsyncProvider::Usable() const {
  static const bool env_disabled = []() {
    const char* env = std::getenv("IVI_WL_VSYNC");
    return env != nullptr && std::string_view(env) == "0";
  }();
  return !env_disabled && presentation_ != nullptr &&
         clock_compatible_.load(std::memory_order_acquire);
}

bool WaylandVsyncProvider::GridPacingEnabledByEnv() {
  const char* env = std::getenv("IVI_WL_VSYNC_GRID");
  return !(env != nullptr && std::string_view(env) == "0");
}

void WaylandVsyncProvider::SetGridPacing(const bool grid) {
  grid_wanted_.store(grid, std::memory_order_release);
  ApplyGrid(grid && last_refresh_ns_.load(std::memory_order_acquire) != 0);
}

void WaylandVsyncProvider::ApplyGrid(const bool on) {
  grid_.store(on, std::memory_order_release);
  SetPaceToVblank(on);
  SetPaceFlips(on);
  SetPaceKeepPhase(on);
  SetPaceStartWindow(on ? start_window_ns_.load(std::memory_order_acquire) : 0);
}

void WaylandVsyncProvider::SetStartWindow(const uint64_t window_ns) {
  start_window_ns_.store(window_ns, std::memory_order_release);
  if (grid_.load(std::memory_order_acquire)) {
    SetPaceStartWindow(window_ns);  // off the grid, ApplyGrid picks it up
  }
}

void WaylandVsyncProvider::SetGridDepth(const size_t depth) {
  grid_depth_.store(std::clamp<size_t>(depth, 1, kGridInFlight),
                    std::memory_order_release);
}

void WaylandVsyncProvider::NoteLatch(const uint64_t take_ns) {
  static const bool disabled = [] {
    const char* env = std::getenv("IVI_WL_LATCH_LEAD");
    return env != nullptr && std::string_view(env) == "0";
  }();
  const uint64_t period = PeriodNs();
  const uint64_t last = last_present_ns_.load(std::memory_order_acquire);
  if (disabled || last == 0 || take_ns < last || period == 0) {
    return;
  }
  // Views on one display share this provider, each sampled on its own
  // rasterizer thread.
  const std::lock_guard<std::mutex> lock(latch_mu_);
  if (last_take_ns_ != 0 && take_ns >= last_take_ns_ &&
      take_ns - last_take_ns_ < period / 4) {
    return;  // another layer of the same frame
  }
  last_take_ns_ = take_ns;
  // The phase is on a circle: a take just after a vblank leads by almost a
  // whole period, which is right -- the producer must be ready by then.
  const uint64_t lead = period - (take_ns - last) % period;
  uint64_t cur = latch_lead_ns_.load(std::memory_order_relaxed);
  if (period != latch_period_ns_) {
    latch_period_ns_ = period;  // a new mode: what was learned no longer holds
    cur = 0;
  }
  const uint64_t next =
      (cur == 0 || lead >= cur) ? lead : cur - (cur - lead) / kLatchLeadDecay;
  latch_lead_ns_.store(static_cast<uint32_t>(std::min<uint64_t>(next, period)),
                       std::memory_order_release);
}

// SetEngine + SubmitBaton are inherited from IVsyncProvider (the shared
// park/drain/marshal machinery). IsSourcePending()/PeriodNs() supply the
// Wayland-specific bits.

uint64_t WaylandVsyncProvider::RequestFeedback() {
  if (!Usable() || surface_ == nullptr) {
    return 0;
  }
  {
    std::lock_guard<std::mutex> lock(feedback_mu_);
    if (feedback_in_flight_.size() >= kFeedbackInFlightCap) {
      ihs::log::warn(
          "[WaylandVsync] feedback_in_flight_ saturated at {}; compositor "
          "dropping wp_presentation_feedback requests",
          feedback_in_flight_.size());
      return 0;
    }
  }
  // feedback(surface, callback:new_id) — the new_id is the TRAILING arg, so
  // construct_at_end (not construct, which puts new_id first). Bound to the
  // next commit; must be issued BEFORE wl_surface.commit / eglSwapBuffers.
  auto* raw = wl::construct_at_end<ptc::wp_presentation_feedback_traits,
                                   ptc::wp_presentation_traits::Op::Feedback>(
      *presentation_, reinterpret_cast<wl_proxy*>(surface_));
  if (raw == nullptr) {
    ihs::log::warn("[WaylandVsync] wp_presentation.feedback returned null");
    return 0;
  }
  auto handler = std::make_unique<WpFeedbackHandler>(this);
  // Stamp this frame's input cutoff: the frame being committed now was driven
  // by the last baton we delivered, so its frame_start_time is the cutoff of
  // the inputs it consumed. OnPresented reads it back for the motion-to-photon
  // frame-accurate drain. Without grid pacing a present in flight keeps the
  // next baton parked, so this is exact. On the grid the next baton can go out
  // before this frame commits, and the cutoff can belong to the frame after
  // it: the profile is then approximate.
  handler->cutoff_ns_ = LastDeliveredFrameStartNs();
  const uint64_t serial =
      feedback_serial_.fetch_add(1, std::memory_order_relaxed) + 1;
  handler->serial_ = serial;
  handler->_SetProxy(raw);  // adopts + installs presented/discarded listener
  {
    std::lock_guard<std::mutex> lock(feedback_mu_);
    feedback_in_flight_.push_back(std::move(handler));
    in_flight_.store(feedback_in_flight_.size(), std::memory_order_release);
  }
  feedback_pending_.store(true, std::memory_order_release);
  return serial;
}

void WaylandVsyncProvider::RetireFeedback(WpFeedbackHandler* fb) {
  // Race-safe: only the path that removes `fb` from the vector destroys it (the
  // unique_ptr's dtor). If Stop() swapped the vector out, IT owns destruction.
  std::unique_ptr<WpFeedbackHandler> owned;
  bool empty = false;
  {
    std::lock_guard<std::mutex> lock(feedback_mu_);
    auto& vec = feedback_in_flight_;
    auto it = std::find_if(vec.begin(), vec.end(),
                           [fb](const std::unique_ptr<WpFeedbackHandler>& h) {
                             return h.get() == fb;
                           });
    if (it != vec.end()) {
      owned = std::move(*it);  // hold past the lock; destroy outside
      vec.erase(it);
    }
    empty = vec.empty();
    in_flight_.store(vec.size(), std::memory_order_release);
  }
  if (empty) {
    feedback_pending_.store(false, std::memory_order_release);
  }
  // Pairs with SubmitBaton's fence: the presentation that follows sees a
  // baton it parked, or SubmitBaton sees this retirement.
  std::atomic_thread_fence(std::memory_order_seq_cst);
  // owned destructs here (outside the lock), destroying the wl_proxy.
}

void WaylandVsyncProvider::OnPresented(const uint64_t present_ns,
                                       const uint32_t refresh_ns,
                                       const uint64_t msc,
                                       const uint32_t flags,
                                       WpFeedbackHandler* fb) {
  if (refresh_ns > 0 && refresh_ns <= kMaxPlausibleRefreshNs) {
    last_refresh_ns_.store(refresh_ns, std::memory_order_release);
  }
  last_present_ns_.store(present_ns, std::memory_order_release);
  // Grid pacing waits for a real refresh period: refresh 0 (unknown, or a
  // variable rate) leaves presentation-paced frames.
  if (!grid_.load(std::memory_order_acquire) &&
      grid_wanted_.load(std::memory_order_acquire) &&
      last_refresh_ns_.load(std::memory_order_acquire) != 0) {
    ApplyGrid(true);
  }
  // Read the frame's input cutoff and serial before RetireFeedback destroys
  // fb.
  const uint64_t cutoff_ns = fb->cutoff_ns_;
  const uint64_t serial = fb->serial_;
  RetireFeedback(
      fb);  // clears feedback_pending_ once the last feedback retires
  // Motion-to-photon: this scanout timestamp closes out the input events this
  // frame consumed — those at or before its cutoff (event thread, same as
  // RecordInput). present_ns feeds the floor; cutoff_ns the frame-accurate
  // pass.
  if (m2p_ != nullptr) {
    m2p_->RecordPresent(present_ns, cutoff_ns, "wayland");
  }
  // The platform views this commit showed. The host's timing and flags are the
  // shell window's -- except ZERO_COPY, which says the host scanned out the
  // window's buffer, not that a view's own buffer reached the screen; the
  // shell composited the views into it.
  std::shared_ptr<PresentationTracker> tracker;
  {
    const std::lock_guard<std::mutex> lock(tracker_mu_);
    tracker = tracker_;
  }
  if (tracker) {
    PresentationTime when;
    when.ust_ns = present_ns;
    when.refresh_ns = refresh_ns;
    when.msc = msc;
    when.flags = flags & ~kPresentedZeroCopy;
    tracker->Presented(serial, when);
  }
  // Base records the present (cadence profile) + hands the parked baton back
  // with the compositor's real presented timestamp.
  DeliverVsync(present_ns);
}

void WaylandVsyncProvider::OnDiscarded(WpFeedbackHandler* fb) {
  // The host dropped this commit without showing it -- the window is hidden,
  // or a later commit replaced it first. Nothing to report, but what it used
  // is free, which is what keeps an occluded shell's producers from stalling.
  const uint64_t serial = fb->serial_;
  RetireFeedback(fb);
  std::shared_ptr<PresentationTracker> tracker;
  {
    const std::lock_guard<std::mutex> lock(tracker_mu_);
    tracker = tracker_;
  }
  if (tracker) {
    tracker->Discarded(serial);
  }
  DeliverDiscard();  // base records the discard + returns the baton (wall
                     // clock)
}

void WaylandVsyncProvider::Stop() {
  std::vector<std::unique_ptr<WpFeedbackHandler>> drained;
  {
    std::lock_guard<std::mutex> lock(feedback_mu_);
    drained.swap(feedback_in_flight_);
  }
  drained.clear();  // destroys each feedback proxy, canceling its listener
  feedback_pending_.store(false, std::memory_order_release);
  in_flight_.store(0, std::memory_order_release);
  last_present_ns_.store(0, std::memory_order_release);
  {
    const std::lock_guard<std::mutex> lock(latch_mu_);
    last_take_ns_ = 0;
    latch_period_ns_ = 0;
  }
  latch_lead_ns_.store(0, std::memory_order_release);

  // Base drops the parked baton, clears engine/runner, and logs the session
  // cadence summary.
  IVsyncProvider::Stop();
}

}  // namespace ivi
