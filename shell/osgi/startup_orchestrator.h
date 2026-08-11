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

#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "bundle_host.h"
#include "bundle_state.h"
#include "startup_plan.h"

namespace ihs::osgi {

// Why a bundle did not come up. Ordered from earliest failure to latest.
enum class StartupFailure : uint8_t {
  kNone,
  kSpawnFailed,      // the host could not create or run an engine
  kAffinityRefused,  // pthread_setaffinity_np refused a validated core
  kTimedOut,         // never reported ACTIVE within startup_timeout_ms
};

std::string_view StartupFailureName(StartupFailure failure);

// What happened to one bundle during startup.
struct BundleOutcome {
  std::string symbolic_name;
  Priority priority{Priority::kNormal};
  BundleState state{BundleState::kInstalled};
  StartupFailure failure{StartupFailure::kNone};

  [[nodiscard]] bool ok() const { return failure == StartupFailure::kNone; }
};

// Brings bundles up in the order BuildStartupPlan decided, and is the only
// thing that blocks.
//
// Critical bundles are spawned and awaited one at a time before the asio
// reactor runs, because that is what "critical" buys: an instrument cluster is
// on screen before anything else competes for the GPU. Everything after the
// reactor starts is launched without being awaited -- a normal bundle that is
// slow delays only itself, and a background service has no view to delay.
//
// Reaching ACTIVE is not something the shell can observe directly; the bundle's
// Dart activator reports it over the bridge, which calls NotifyActive. The wait
// is therefore a condition variable with a deadline rather than a poll, and the
// deadline is per bundle from its manifest.
class BundleStartupOrchestrator {
 public:
  BundleStartupOrchestrator(IBundleHost& host,
                            std::vector<BundleManifest> manifests,
                            const StartupPolicy& policy = {});

  BundleStartupOrchestrator(const BundleStartupOrchestrator&) = delete;
  BundleStartupOrchestrator& operator=(const BundleStartupOrchestrator&) =
      delete;

  [[nodiscard]] const StartupPlan& plan() const { return plan_; }

  // Spawn each critical bundle and wait for it to report ACTIVE, in plan order.
  // Blocks for at most plan().CriticalBudget(). Runs before the reactor.
  //
  // A bundle that fails is torn down and recorded, and startup continues: one
  // broken cluster must not take the rest of the system with it. The caller
  // decides what a failed critical bundle means for the product.
  std::vector<BundleOutcome> StartCriticalPhase();

  // Launch the normal (staggered) and background (immediate) phases without
  // waiting for any of them. Runs after the reactor is up.
  std::vector<BundleOutcome> StartDeferredPhases();

  // The bundle's activator reported ACTIVE, via the bridge. Safe from any
  // thread; unblocks a critical wait.
  void NotifyActive(const std::string& symbolic_name);

  // The bundle stopped or died. Moves it out of the live states so the same
  // symbolic name can be started again.
  void NotifyStopped(const std::string& symbolic_name);

  [[nodiscard]] BundleState StateOf(const std::string& symbolic_name) const;

  // Engine handle for a live bundle, or kInvalidBundleHandle.
  [[nodiscard]] BundleHandle HandleOf(const std::string& symbolic_name) const;

  // Bundles currently in STARTING or ACTIVE.
  [[nodiscard]] std::vector<std::string> LiveBundles() const;

 private:
  // Spawn + pin + advance to STARTING. Returns the failure reason, leaving the
  // bundle torn down and non-live on anything but kNone.
  StartupFailure Launch(const PlannedBundle& planned, BundleHandle& out_handle);

  // Wait for @planned to report ACTIVE within its deadline. Caller must not
  // hold mutex_.
  bool AwaitActive(const PlannedBundle& planned);

  // Caller holds mutex_.
  BundleStateMachine* FindLocked(const std::string& symbolic_name);

  // Return a failed or stopped bundle to RESOLVED and drop its handle, so the
  // same symbolic name can be started again. Caller holds mutex_.
  void UnwindLocked(const std::string& symbolic_name);

  IBundleHost& host_;
  std::vector<BundleManifest> manifests_;
  StartupPlan plan_;

  mutable std::mutex mutex_;
  // Signalled by NotifyActive/NotifyStopped so a critical wait wakes early
  // rather than always burning its full deadline.
  std::condition_variable cv_;

  struct Live {
    BundleStateMachine machine;
    BundleHandle handle{kInvalidBundleHandle};
  };
  // Keyed by symbolic name, which osgi_config already guarantees is unique.
  std::map<std::string, Live> bundles_;
};

}  // namespace ihs::osgi
