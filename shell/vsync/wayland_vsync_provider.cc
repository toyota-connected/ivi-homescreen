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

#include <wayland-client.h>

#include <wl/client_helpers.hpp>  // wl::SetupHandler
#include <wl/proxy_impl.hpp>      // wl::construct

#include "asio/post.hpp"

#include "libflutter_engine.h"
#include "logging/logging.h"
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
}  // namespace

// Handler method bodies — the classes are defined in the header so unique_ptr
// members are destructible in any TU; the bodies live here where the provider
// is complete.
void WaylandVsyncProvider::PresentationHandler::OnClockId(const uint32_t clk_id) {
  owner_->clock_id_.store(static_cast<clockid_t>(clk_id),
                          std::memory_order_release);
  // FlutterEngineGetCurrentTime is CLOCK_MONOTONIC; only then are the
  // compositor's presented timestamps usable as frame_start_time directly.
  owner_->clock_compatible_.store(clk_id == CLOCK_MONOTONIC,
                                  std::memory_order_release);
  spdlog::debug("[WaylandVsync] wp_presentation clock_id={}", clk_id);
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
                                    uint32_t /*seq_hi*/,
                                    uint32_t /*seq_lo*/,
                                    uint32_t /*flags*/) {
  // Reject malformed tv_sec_hi: CLOCK_MONOTONIC seconds-since-boot has never
  // reached 2^32 (~136 years); a non-zero high word would overflow the *1e9
  // below. Fall back to the discarded path (still returns the baton).
  if (tv_sec_hi != 0) {
    owner_->OnDiscarded(this);
    return;
  }
  const uint64_t present_ns =
      static_cast<uint64_t>(tv_sec_lo) * 1'000'000'000ULL + tv_nsec;
  owner_->OnPresented(present_ns, refresh, this);
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
  auto* raw = wl_registry_bind(registry, name,
                               &ptc::wp_presentation_traits::wl_iface(),
                               bind_ver);
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
  spdlog::debug("[WaylandVsync] bound wp_presentation v{}", bind_ver);
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

// SetEngine + SubmitBaton are inherited from IVsyncProvider (the shared
// park/drain/marshal machinery). IsSourcePending()/PeriodNs() supply the
// Wayland-specific bits.

void WaylandVsyncProvider::RequestFeedback() {
  if (!Usable() || surface_ == nullptr) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(feedback_mu_);
    if (feedback_in_flight_.size() >= kFeedbackInFlightCap) {
      spdlog::warn(
          "[WaylandVsync] feedback_in_flight_ saturated at {}; compositor "
          "dropping wp_presentation_feedback requests",
          feedback_in_flight_.size());
      return;
    }
  }
  // feedback(surface, callback:new_id) — the new_id is the TRAILING arg, so
  // construct_at_end (not construct, which puts new_id first). Bound to the
  // next commit; must be issued BEFORE wl_surface.commit / eglSwapBuffers.
  auto* raw = wl::construct_at_end<ptc::wp_presentation_feedback_traits,
                                   ptc::wp_presentation_traits::Op::Feedback>(
      *presentation_, reinterpret_cast<wl_proxy*>(surface_));
  if (raw == nullptr) {
    spdlog::warn("[WaylandVsync] wp_presentation.feedback returned null");
    return;
  }
  auto handler = std::make_unique<WpFeedbackHandler>(this);
  handler->_SetProxy(raw);  // adopts + installs presented/discarded listener
  {
    std::lock_guard<std::mutex> lock(feedback_mu_);
    feedback_in_flight_.push_back(std::move(handler));
  }
  feedback_pending_.store(true, std::memory_order_release);
}

void WaylandVsyncProvider::RetireFeedback(WpFeedbackHandler* fb) {
  // Race-safe: only the path that removes `fb` from the vector destroys it (the
  // unique_ptr's dtor). If Stop() swapped the vector out, IT owns destruction.
  std::unique_ptr<WpFeedbackHandler> owned;
  bool empty = false;
  {
    std::lock_guard<std::mutex> lock(feedback_mu_);
    auto& vec = feedback_in_flight_;
    auto it = std::find_if(
        vec.begin(), vec.end(),
        [fb](const std::unique_ptr<WpFeedbackHandler>& h) { return h.get() == fb; });
    if (it != vec.end()) {
      owned = std::move(*it);  // hold past the lock; destroy outside
      vec.erase(it);
    }
    empty = vec.empty();
  }
  if (empty) {
    feedback_pending_.store(false, std::memory_order_release);
  }
  // owned destructs here (outside the lock), destroying the wl_proxy.
}

void WaylandVsyncProvider::OnPresented(const uint64_t present_ns,
                                       const uint32_t refresh_ns,
                                       WpFeedbackHandler* fb) {
  if (refresh_ns > 0 && refresh_ns <= kMaxPlausibleRefreshNs) {
    last_refresh_ns_.store(refresh_ns, std::memory_order_release);
  }
  RetireFeedback(fb);  // clears feedback_pending_ once the last feedback retires
  // Base records the present (cadence profile) + hands the parked baton back
  // with the compositor's real presented timestamp.
  DeliverVsync(present_ns);
}

void WaylandVsyncProvider::OnDiscarded(WpFeedbackHandler* fb) {
  RetireFeedback(fb);
  DeliverDiscard();  // base records the discard + returns the baton (wall clock)
}

void WaylandVsyncProvider::Stop() {
  std::vector<std::unique_ptr<WpFeedbackHandler>> drained;
  {
    std::lock_guard<std::mutex> lock(feedback_mu_);
    drained.swap(feedback_in_flight_);
  }
  drained.clear();  // destroys each feedback proxy, cancelling its listener
  feedback_pending_.store(false, std::memory_order_release);

  // Base drops the parked baton, clears engine/runner, and logs the session
  // cadence summary.
  IVsyncProvider::Stop();
}

}  // namespace ivi
