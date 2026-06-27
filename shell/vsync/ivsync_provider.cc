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

#include "asio/post.hpp"

#include "libflutter_engine.h"
#include "task_runner.h"

namespace ivi {

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
  // Idle pipeline (first frame / post-resume / waking on input) — nothing will
  // fire a source event, so kick the baton inline (when the runner is wired).
  DrainParkedBaton();
}

void IVsyncProvider::EnableProfile(const bool enabled, std::string label) {
  profile_enabled_ = enabled;
  profile_label_ = std::move(label);
}

void IVsyncProvider::DeliverVsync(const uint64_t frame_start_ns) {
  if (profile_enabled_) {
    // Real present — record the inter-present interval from its timestamp.
    profile_.Record(profile_label_, /*ok=*/true, frame_start_ns);
  }
  DeliverBaton(frame_start_ns);
}

void IVsyncProvider::DeliverDiscard() {
  if (profile_enabled_) {
    profile_.Record(profile_label_, /*ok=*/false);  // counts a discard
  }
  // Still hand the baton back (wall clock) so Flutter keeps scheduling.
  DeliverBaton(LibFlutterEngine->GetCurrentTime());
}

bool IVsyncProvider::DeliverParkedBaton() {
  const intptr_t baton = vsync_baton_.exchange(0, std::memory_order_acq_rel);
  if (baton == 0) {
    return false;
  }
  auto* engine = engine_.load(std::memory_order_acquire);
  if (engine != nullptr) {
    PostOnVsync(engine, baton, LibFlutterEngine->GetCurrentTime());
  }
  return true;
}

void IVsyncProvider::DeliverBaton(const uint64_t now_ns) {
  const intptr_t baton = vsync_baton_.exchange(0, std::memory_order_acq_rel);
  if (baton == 0) {
    return;
  }
  auto* engine = engine_.load(std::memory_order_acquire);
  if (engine != nullptr) {
    PostOnVsync(engine, baton, now_ns);
  }
}

void IVsyncProvider::DrainParkedBaton() {
  auto* runner = runner_.load(std::memory_order_acquire);
  if (runner == nullptr || runner->GetStrandContext() == nullptr) {
    return;  // leave parked; SetEngine's kick latch drains it once wired (#210)
  }
  const intptr_t baton = vsync_baton_.exchange(0, std::memory_order_acq_rel);
  if (baton == 0) {
    return;
  }
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

  if (profile_enabled_ && !summary_logged_) {
    summary_logged_ = true;
    profile_.LogSessionSummary(profile_label_);
  }
}

}  // namespace ivi
