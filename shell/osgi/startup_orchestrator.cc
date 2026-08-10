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

#include "startup_orchestrator.h"

#include <thread>
#include <utility>

#include "logging/logging.h"

namespace ihs::osgi {

std::string_view StartupFailureName(const StartupFailure failure) {
  switch (failure) {
    case StartupFailure::kNone:
      return "none";
    case StartupFailure::kSpawnFailed:
      return "spawn failed";
    case StartupFailure::kAffinityRefused:
      return "affinity refused";
    case StartupFailure::kTimedOut:
      return "timed out";
  }
  return "unknown";
}

BundleStartupOrchestrator::BundleStartupOrchestrator(
    IBundleHost& host,
    std::vector<BundleManifest> manifests,
    const StartupPolicy& policy)
    : host_(host),
      manifests_(std::move(manifests)),
      plan_(BuildStartupPlan(manifests_, policy)) {
  for (const auto& manifest : manifests_) {
    bundles_.emplace(
        manifest.symbolic_name,
        Live{BundleStateMachine(manifest.symbolic_name), kInvalidBundleHandle});
  }
}

BundleStateMachine* BundleStartupOrchestrator::FindLocked(
    const std::string& symbolic_name) {
  const auto it = bundles_.find(symbolic_name);
  return it == bundles_.end() ? nullptr : &it->second.machine;
}

void BundleStartupOrchestrator::UnwindLocked(const std::string& symbolic_name) {
  const auto it = bundles_.find(symbolic_name);
  if (it == bundles_.end()) {
    return;
  }
  // Back to RESOLVED through STOPPING, the same path a healthy stop takes, so a
  // failed bundle can be started again rather than being stranded.
  if (it->second.machine.IsLive()) {
    it->second.machine.Transition(BundleState::kStopping);
  }
  if (it->second.machine.state() == BundleState::kStopping) {
    it->second.machine.Transition(BundleState::kResolved);
  }
  it->second.handle = kInvalidBundleHandle;
}

StartupFailure BundleStartupOrchestrator::Launch(const PlannedBundle& planned,
                                                 BundleHandle& out_handle) {
  out_handle = kInvalidBundleHandle;
  const BundleManifest& manifest = manifests_[planned.manifest_index];

  {
    std::lock_guard lock(mutex_);
    auto* machine = FindLocked(planned.symbolic_name);
    if (machine == nullptr) {
      return StartupFailure::kSpawnFailed;
    }
    // INSTALLED -> RESOLVED happens here: by this point the manifest parsed and
    // its config resolved, which is exactly what RESOLVED means.
    if (machine->state() == BundleState::kInstalled) {
      machine->Transition(BundleState::kResolved);
    }
    // STARTING is entered BEFORE the spawn, not after it returns. The engine
    // runs the activator on its own thread, so NotifyActive can land while
    // Spawn is still on the stack -- entering STARTING afterwards would leave a
    // window where the bundle's own ACTIVE report is rejected as coming from
    // the wrong state, and the bundle would then sit out its whole deadline and
    // be torn down despite having started correctly.
    if (!machine->Transition(BundleState::kStarting)) {
      return StartupFailure::kSpawnFailed;
    }
  }

  const BundleHandle handle = host_.Spawn(manifest);
  if (handle == kInvalidBundleHandle) {
    ihs::log::error("[osgi] bundle '{}': engine spawn failed",
                    planned.symbolic_name);
    std::lock_guard lock(mutex_);
    UnwindLocked(planned.symbolic_name);
    return StartupFailure::kSpawnFailed;
  }

  if (planned.cpu_core != -1 && !host_.PinThread(handle, planned.cpu_core)) {
    // The core already passed validation against the process affinity mask, so
    // a refusal here is a runtime condition, not a config error. Treated as
    // fatal to the bundle rather than ignored: cpu_core was written to buy a
    // scheduling guarantee, and running on without it silently withdraws that
    // guarantee while the bundle still looks healthy.
    ihs::log::error("[osgi] bundle '{}': could not pin engine thread to CPU {}",
                    planned.symbolic_name, planned.cpu_core);
    host_.Shutdown(handle);
    std::lock_guard lock(mutex_);
    UnwindLocked(planned.symbolic_name);
    return StartupFailure::kAffinityRefused;
  }

  {
    std::lock_guard lock(mutex_);
    auto it = bundles_.find(planned.symbolic_name);
    if (it != bundles_.end()) {
      it->second.handle = handle;
    }
  }
  out_handle = handle;
  return StartupFailure::kNone;
}

bool BundleStartupOrchestrator::AwaitActive(const PlannedBundle& planned) {
  std::unique_lock lock(mutex_);
  // Predicate rather than a bare timed wait: NotifyActive may land before the
  // wait begins (a fast bundle, or a host that reports synchronously), and a
  // bare wait would then sit here for the whole deadline having already missed
  // the signal.
  cv_.wait_for(lock, planned.startup_timeout, [&] {
    const auto it = bundles_.find(planned.symbolic_name);
    if (it == bundles_.end()) {
      return true;
    }
    const BundleState state = it->second.machine.state();
    // Stop waiting on ACTIVE, and also on anything that left the live states --
    // a bundle that died during startup is never going to signal.
    return state == BundleState::kActive || !it->second.machine.IsLive();
  });

  // The predicate fires for two different reasons and only one of them is
  // success: a bundle that died during startup also leaves the live states.
  // Returning wait_for's result directly would report a crashed bundle as
  // having started, which is the worst of the three outcomes to get wrong.
  const auto it = bundles_.find(planned.symbolic_name);
  return it != bundles_.end() &&
         it->second.machine.state() == BundleState::kActive;
}

std::vector<BundleOutcome> BundleStartupOrchestrator::StartCriticalPhase() {
  std::vector<BundleOutcome> outcomes;
  outcomes.reserve(plan_.critical.size());

  if (!plan_.critical.empty()) {
    ihs::log::info(
        "[osgi] starting {} critical bundle(s); worst case {} ms before the "
        "reactor runs",
        plan_.critical.size(), plan_.CriticalBudget().count());
  }

  for (const auto& planned : plan_.critical) {
    BundleOutcome outcome;
    outcome.symbolic_name = planned.symbolic_name;
    outcome.priority = planned.priority;

    BundleHandle handle = kInvalidBundleHandle;
    outcome.failure = Launch(planned, handle);

    if (outcome.failure == StartupFailure::kNone && !AwaitActive(planned)) {
      // Deadline expired with no ACTIVE. Tear the engine down rather than leave
      // a half-started bundle holding a view and a vsync registration.
      ihs::log::error(
          "[osgi] bundle '{}': did not reach ACTIVE within {} ms; tearing down",
          planned.symbolic_name, planned.startup_timeout.count());
      host_.Shutdown(handle);
      outcome.failure = StartupFailure::kTimedOut;
    }

    {
      std::lock_guard lock(mutex_);
      if (outcome.failure != StartupFailure::kNone) {
        UnwindLocked(planned.symbolic_name);
      }
      const auto it = bundles_.find(planned.symbolic_name);
      if (it != bundles_.end()) {
        outcome.state = it->second.machine.state();
      }
    }

    // Deliberately not fatal: one broken cluster must not take the rest of the
    // system with it. What a failed critical bundle means for the product is
    // the caller's decision, which is why the outcome is returned rather than
    // acted on here.
    outcomes.push_back(std::move(outcome));
  }
  return outcomes;
}

std::vector<BundleOutcome> BundleStartupOrchestrator::StartDeferredPhases() {
  std::vector<BundleOutcome> outcomes;
  outcomes.reserve(plan_.normal.size() + plan_.background.size());

  std::chrono::milliseconds elapsed{0};
  for (const auto& planned : plan_.normal) {
    // The stagger keeps several engines from contending for the same
    // first-frame resources -- disk for the AOT snapshot, GPU for the first
    // pipeline compile. Sleeping is honest here: this phase runs after the
    // reactor is up, so nothing is being starved by the wait.
    if (planned.start_delay > elapsed) {
      std::this_thread::sleep_for(planned.start_delay - elapsed);
      elapsed = planned.start_delay;
    }

    BundleOutcome outcome;
    outcome.symbolic_name = planned.symbolic_name;
    outcome.priority = planned.priority;
    BundleHandle handle = kInvalidBundleHandle;
    outcome.failure = Launch(planned, handle);
    outcome.state = StateOf(planned.symbolic_name);
    outcomes.push_back(std::move(outcome));
  }

  for (const auto& planned : plan_.background) {
    BundleOutcome outcome;
    outcome.symbolic_name = planned.symbolic_name;
    outcome.priority = planned.priority;
    BundleHandle handle = kInvalidBundleHandle;
    outcome.failure = Launch(planned, handle);
    outcome.state = StateOf(planned.symbolic_name);
    outcomes.push_back(std::move(outcome));
  }

  // Neither phase is awaited: a slow normal bundle delays only itself, and a
  // background service has no view to delay.
  return outcomes;
}

void BundleStartupOrchestrator::NotifyActive(const std::string& symbolic_name) {
  {
    std::lock_guard lock(mutex_);
    auto* machine = FindLocked(symbolic_name);
    if (machine == nullptr) {
      ihs::log::warn("[osgi] ACTIVE reported by unknown bundle '{}'",
                     symbolic_name);
      return;
    }
    if (machine->state() != BundleState::kStarting) {
      // Either a duplicate report or one from a bundle that already gave up.
      // Transition would reject it anyway; saying so is more useful.
      ihs::log::warn("[osgi] bundle '{}' reported ACTIVE while {}",
                     symbolic_name, BundleStateName(machine->state()));
      return;
    }
    machine->Transition(BundleState::kActive);
  }
  cv_.notify_all();
}

void BundleStartupOrchestrator::NotifyStopped(
    const std::string& symbolic_name) {
  {
    std::lock_guard lock(mutex_);
    UnwindLocked(symbolic_name);
  }
  // Wakes a critical wait that would otherwise sit out its full deadline for a
  // bundle that is already gone.
  cv_.notify_all();
}

BundleState BundleStartupOrchestrator::StateOf(
    const std::string& symbolic_name) const {
  std::lock_guard lock(mutex_);
  const auto it = bundles_.find(symbolic_name);
  return it == bundles_.end() ? BundleState::kUninstalled
                              : it->second.machine.state();
}

BundleHandle BundleStartupOrchestrator::HandleOf(
    const std::string& symbolic_name) const {
  std::lock_guard lock(mutex_);
  const auto it = bundles_.find(symbolic_name);
  return it == bundles_.end() ? kInvalidBundleHandle : it->second.handle;
}

std::vector<std::string> BundleStartupOrchestrator::LiveBundles() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> live;
  for (const auto& [name, bundle] : bundles_) {
    if (bundle.machine.IsLive()) {
      live.push_back(name);
    }
  }
  return live;
}

}  // namespace ihs::osgi
