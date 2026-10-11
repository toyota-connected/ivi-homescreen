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

#include "vsync/ivsync_provider.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string_view>

#include "asio/post.hpp"
#include "asio/steady_timer.hpp"

#include "libflutter_engine.h"
#include "task_runner.h"

namespace ivi {

// The deferred-delivery timer, shared with its handler so the handler can find
// out, under the lock, whether the provider is still there.
struct IVsyncProvider::PaceTimer {
  std::mutex mu;
  IVsyncProvider* owner{nullptr};
  std::unique_ptr<asio::steady_timer> timer;  // runner thread only
};

bool IVsyncProvider::PaceEnabledByEnv() {
  const char* env = std::getenv("IVI_VSYNC_PACE");
  return !(env != nullptr && std::string_view(env) == "0");
}

IVsyncProvider::~IVsyncProvider() {
  if (pace_timer_) {
    const std::lock_guard<std::mutex> lock(pace_timer_->mu);
    pace_timer_->owner = nullptr;
  }
}

uint64_t IVsyncProvider::NowNs() const {
  if (now_for_test_) {
    return now_for_test_();
  }
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

uint64_t IVsyncProvider::NextVblankAfter(const uint64_t t_ns) const {
  const uint64_t period = PeriodNs();
  const uint64_t last = last_vblank_ns_.load(std::memory_order_acquire);
  if (period == 0) {
    return t_ns;
  }
  if (last == 0 || t_ns < last) {
    return t_ns + period;
  }
  return last + ((t_ns - last) / period + 1) * period;
}

void IVsyncProvider::NoteHandOut(const uint64_t at_ns) {
  if (pace_.load(std::memory_order_acquire)) {
    next_allowed_ns_.store(NextVblankAfter(at_ns), std::memory_order_release);
  }
}

void IVsyncProvider::ScheduleDeferred(const uint64_t due_ns) {
  if (deferred_due_ns_.exchange(due_ns, std::memory_order_acq_rel) == due_ns) {
    return;  // already armed for that vblank
  }
  if (schedule_for_test_) {
    schedule_for_test_(due_ns);
    return;
  }
  auto* runner = runner_.load(std::memory_order_acquire);
  if (runner == nullptr || runner->GetIoContext() == nullptr ||
      runner->GetStrandContext() == nullptr) {
    deferred_due_ns_.store(0, std::memory_order_release);
    return;  // the kick latch delivers it once the runner is wired
  }
  if (!pace_timer_) {
    pace_timer_ = std::make_shared<PaceTimer>();
    pace_timer_->owner = this;
  }
  auto state = pace_timer_;
  auto* io = runner->GetIoContext();
  asio::post(*runner->GetStrandContext(), [state, io, due_ns]() {
    if (!state->timer) {
      state->timer = std::make_unique<asio::steady_timer>(*io);
    }
    // Both clocks are CLOCK_MONOTONIC; express the due time as a delay.
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
                         static_cast<uint64_t>(ts.tv_nsec);
    const auto delay =
        std::chrono::nanoseconds(due_ns > now ? due_ns - now : 0);
    state->timer->expires_after(delay);
    state->timer->async_wait([state, due_ns](const asio::error_code& ec) {
      if (ec) {
        return;  // re-armed for another vblank, or shutting down
      }
      const std::lock_guard<std::mutex> lock(state->mu);
      if (state->owner != nullptr) {
        state->owner->OnDeferredDue(due_ns);
      }
    });
  });
}

void IVsyncProvider::OnDeferredDue(const uint64_t due_ns) {
  // Only the delivery still asked for: a flip may have handed the baton back
  // meanwhile, and re-armed or cleared this.
  if (due_ns == 0 ||
      deferred_due_ns_.load(std::memory_order_acquire) != due_ns) {
    return;
  }
  deferred_due_ns_.store(0, std::memory_order_release);
  if (IsSourcePending()) {
    return;  // a present went out meanwhile: its flip returns the baton
  }
  DeliverBaton(due_ns);  // the vblank is this frame's start
}

void IVsyncProvider::SetEngine(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                               TaskRunner* runner) {
  engine_.store(engine, std::memory_order_release);
  runner_.store(runner, std::memory_order_release);
  // Kick latch (#210): the engine's first vsync request can land before the
  // runner is wired, leaving a baton parked. Drain it now that we can deliver.
  DrainParkedBaton();
}

void IVsyncProvider::SubmitBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                                 const intptr_t baton) {
  engine_.store(engine, std::memory_order_release);
  // Store the baton first so a source event racing us (a present landing
  // between the IsSourcePending() check and the store) still finds it.
  vsync_baton_.store(baton, std::memory_order_release);
  if (IsSourcePending()) {
    return;  // a present is in flight; its completion will return the baton
  }
  // Paced: a baton already went out this refresh, so this one waits for the
  // next vblank rather than starting a second frame now.
  if (pace_.load(std::memory_order_acquire)) {
    const uint64_t due = next_allowed_ns_.load(std::memory_order_acquire);
    if (NowNs() < due) {
      ScheduleDeferred(due);
      return;
    }
  }
  // Idle pipeline (first frame / post-resume / waking on input) — nothing will
  // fire a source event, so kick the baton inline (when the runner is wired).
  DrainParkedBaton();
}

void IVsyncProvider::EnableProfile(const bool enabled, std::string label) {
  profile_enabled_ = enabled;
  profile_label_ = std::move(label);
}

void IVsyncProvider::DeliverVsync(const uint64_t frame_start_ns) {
  if (pace_.load(std::memory_order_acquire)) {
    last_vblank_ns_.store(frame_start_ns, std::memory_order_release);
    deferred_due_ns_.store(0, std::memory_order_release);  // the flip delivers
  }
  if (profile_enabled_) {
    // Real present — record the inter-present interval from its timestamp.
    profile_.Record(profile_label_, /*ok=*/true, frame_start_ns);
  }
  DeliverBaton(frame_start_ns);
}

void IVsyncProvider::DeliverDiscard() {
  if (parked_.load(std::memory_order_acquire)) {
    return;  // parked: the baton stays held, and this is not a dropped frame
  }
  if (profile_enabled_) {
    profile_.Record(profile_label_, /*ok=*/false);  // counts a discard
  }
  // Still hand the baton back (wall clock) so Flutter keeps scheduling.
  DeliverBaton(LibFlutterEngine->GetCurrentTime());
}

bool IVsyncProvider::DeliverParkedBaton() {
  // Check the runner before taking the baton, as DrainParkedBaton does.
  // PostOnVsync silently no-ops without one, so exchanging first would drop
  // the baton on the floor and leave Flutter waiting for a vsync that can
  // never arrive. Leaving it parked is recoverable: SetRunner's kick latch
  // drains it once the runner is wired.
  auto* runner = runner_.load(std::memory_order_acquire);
  if (runner == nullptr || runner->GetStrandContext() == nullptr) {
    return false;
  }
  const intptr_t baton = vsync_baton_.exchange(0, std::memory_order_acq_rel);
  if (baton == 0) {
    return false;
  }
  NoteHandOut(NowNs());
  auto* engine = engine_.load(std::memory_order_acquire);
  if (engine != nullptr) {
    PostOnVsync(engine, baton, LibFlutterEngine->GetCurrentTime());
  }
  return true;
}

void IVsyncProvider::SetParked(const bool parked) {
  if (parked_.exchange(parked, std::memory_order_acq_rel) == parked) {
    return;
  }
  if (!parked) {
    // Hand back whatever the engine was waiting on. If it was idle when we
    // parked there is nothing held, and its next request proceeds normally.
    DeliverParkedBaton();
  }
}

void IVsyncProvider::DeliverBaton(const uint64_t now_ns) {
  if (parked_.load(std::memory_order_acquire)) {
    // Leave it parked. Not returning the baton is what stops frame
    // production; SetParked(false) hands this one back.
    return;
  }
  const intptr_t baton = vsync_baton_.exchange(0, std::memory_order_acq_rel);
  if (baton == 0) {
    return;
  }
  NoteHandOut(now_ns);
  auto* engine = engine_.load(std::memory_order_acquire);
  if (engine != nullptr) {
    PostOnVsync(engine, baton, now_ns);
  }
}

void IVsyncProvider::DrainParkedBaton() {
  if (parked_.load(std::memory_order_acquire)) {
    // Parked. This is the inline kick SubmitBaton takes when no present is in
    // flight, so without this check a fresh request from the engine is served
    // immediately and frame production carries on regardless of the gate --
    // which is most of the traffic, not an edge case.
    return;
  }
  auto* runner = runner_.load(std::memory_order_acquire);
  if (runner == nullptr || runner->GetStrandContext() == nullptr) {
    return;  // leave parked; SetEngine's kick latch drains it once wired (#210)
  }
  const intptr_t baton = vsync_baton_.exchange(0, std::memory_order_acq_rel);
  if (baton == 0) {
    return;
  }
  NoteHandOut(NowNs());
  auto* engine = engine_.load(std::memory_order_acquire);
  if (engine != nullptr) {
    PostOnVsync(engine, baton, LibFlutterEngine->GetCurrentTime());
  }
}

void IVsyncProvider::PostOnVsync(FLUTTER_API_SYMBOL(FlutterEngine) engine,
                                 const intptr_t baton,
                                 const uint64_t now_ns) {
  if (engine == nullptr || baton == 0) {
    return;
  }
  auto* runner = runner_.load(std::memory_order_acquire);
  if (runner == nullptr || runner->GetStrandContext() == nullptr) {
    return;  // runner not wired yet — caller leaves the baton parked
  }
  const uint64_t period_ns = PeriodNs();
  // Record now_ns as this frame's input cutoff (frame_start_time) before we
  // hand Flutter the baton — a source's presentation feedback reads it back to
  // attribute inputs to the frame that consumed them (motion-to-photon).
  last_frame_start_ns_.store(now_ns, std::memory_order_release);
  asio::post(*runner->GetStrandContext(), [engine, baton, now_ns, period_ns]() {
    LibFlutterEngine->OnVsync(engine, baton, now_ns, now_ns + period_ns);
  });
}

void IVsyncProvider::Stop() {
  // Unanswered baton is a documented leak, not a crash — Flutter is shutting
  // down. Clearing engine/runner makes any late source event a no-op.
  vsync_baton_.store(0, std::memory_order_release);
  engine_.store(nullptr, std::memory_order_release);
  runner_.store(nullptr, std::memory_order_release);
  deferred_due_ns_.store(0, std::memory_order_release);
  if (pace_timer_) {
    const std::lock_guard<std::mutex> lock(pace_timer_->mu);
    pace_timer_->owner = nullptr;
  }

  if (profile_enabled_ && !summary_logged_) {
    summary_logged_ = true;
    profile_.LogSessionSummary(profile_label_);
  }
}

}  // namespace ivi
