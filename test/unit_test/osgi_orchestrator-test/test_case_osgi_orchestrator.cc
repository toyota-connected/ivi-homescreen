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

#include <gtest/gtest.h>

#include <chrono>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "osgi/bridge_registry.h"
#include "osgi/startup_orchestrator.h"

namespace {

using ihs::osgi::BundleHandle;
using ihs::osgi::BundleStartupOrchestrator;
using ihs::osgi::BundleState;
using ihs::osgi::kInvalidBundleHandle;
using ihs::osgi::Priority;
using ihs::osgi::StartupFailure;
using std::chrono::milliseconds;

// Deadlines are short so a timeout case costs milliseconds, not half a second.
// The outcome is deterministic either way -- what varies is only how long the
// deadline is, not whether it expires.
constexpr int kShortTimeoutMs = 20;

ihs::osgi::BundleManifest Manifest(const std::string& name,
                                   const Priority priority,
                                   const int cpu_core = -1,
                                   const int timeout_ms = kShortTimeoutMs) {
  ihs::osgi::BundleManifest manifest;
  manifest.symbolic_name = name;
  manifest.priority = priority;
  manifest.cpu_core = cpu_core;
  manifest.startup_timeout_ms = timeout_ms;
  manifest.config.view.bundle_path = "/bundles/" + name;
  return manifest;
}

// A host that records what it was asked to do, and can be told to fail.
//
// `auto_active` makes a spawned bundle report ACTIVE immediately, standing in
// for an activator that comes up promptly. Leaving it off is how a hung bundle
// is modeled -- something a real engine cannot be asked to do on demand.
class FakeHost final : public ihs::osgi::IBundleHost {
 public:
  BundleHandle Spawn(const ihs::osgi::BundleManifest& manifest) override {
    {
      std::lock_guard lock(mutex_);
      spawned_.push_back(manifest.symbolic_name);
      if (spawn_fails_.count(manifest.symbolic_name) != 0) {
        return kInvalidBundleHandle;
      }
    }
    const BundleHandle handle = ++next_handle_;
    if (orchestrator_ != nullptr) {
      const std::string name = manifest.symbolic_name;
      if (async_active_) {
        // The realistic shape: the engine exists, and its activator reports
        // ACTIVE from its own thread at some unpredictable point afterwards.
        // Started here rather than by the test so the report is causally after
        // the spawn, as it always is in production -- an engine cannot report
        // before the shell has created it.
        std::lock_guard lock(mutex_);
        reporters_.emplace_back(
            [this, name] { orchestrator_->NotifyActive(name); });
      } else if (auto_active_) {
        orchestrator_->NotifyActive(name);
      }
    }
    return handle;
  }

  // Must be called while the orchestrator is still alive.
  //
  // The host is declared before the orchestrator (it is passed to the
  // orchestrator's constructor by reference, so it has to outlive it), which
  // means the orchestrator is destroyed FIRST. A reporter thread joined from
  // ~FakeHost would therefore be calling NotifyActive on a destroyed
  // condition_variable. Joining explicitly, before the orchestrator goes out of
  // scope, is the fix; the destructor is only a backstop for a test that
  // forgets.
  void JoinReporters() {
    for (auto& reporter : reporters_) {
      if (reporter.joinable()) {
        reporter.join();
      }
    }
    reporters_.clear();
  }

  ~FakeHost() override { JoinReporters(); }

  bool PinThread(BundleHandle handle, const int cpu_core) override {
    std::lock_guard lock(mutex_);
    pinned_.emplace_back(handle, cpu_core);
    return !pin_refused_;
  }

  void Shutdown(const BundleHandle handle) override {
    std::lock_guard lock(mutex_);
    shutdowns_.push_back(handle);
  }

  void Attach(BundleStartupOrchestrator* orchestrator) {
    orchestrator_ = orchestrator;
  }
  void SetAutoActive(const bool on) { auto_active_ = on; }
  // Report ACTIVE from a thread started inside Spawn, rather than inline.
  void SetAsyncActive(const bool on) { async_active_ = on; }
  void FailSpawnOf(const std::string& name) { spawn_fails_.insert(name); }
  void RefusePinning() { pin_refused_ = true; }

  std::vector<std::string> spawned() const {
    std::lock_guard lock(mutex_);
    return spawned_;
  }
  std::vector<std::pair<BundleHandle, int>> pinned() const {
    std::lock_guard lock(mutex_);
    return pinned_;
  }
  size_t shutdown_count() const {
    std::lock_guard lock(mutex_);
    return shutdowns_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> spawned_;
  std::vector<std::pair<BundleHandle, int>> pinned_;
  std::vector<BundleHandle> shutdowns_;
  std::set<std::string> spawn_fails_;
  BundleHandle next_handle_{0};
  bool auto_active_{true};
  bool async_active_{false};
  bool pin_refused_{false};
  std::vector<std::thread> reporters_;
  BundleStartupOrchestrator* orchestrator_{nullptr};
};

// No stagger: the sleep is real, and the phase ordering under test does not
// depend on it. StartupPlan's own suite covers the arithmetic.
ihs::osgi::StartupPolicy NoStagger() {
  ihs::osgi::StartupPolicy policy;
  policy.normal_stagger = milliseconds{0};
  return policy;
}

}  // namespace

// --- The bridge/orchestrator join -------------------------------------------
//
// Every other test here calls NotifyActive directly, and every test in the
// bridge suite drives the registry directly. Both pass whether or not anything
// connects them -- which is exactly how the two shipped fully built and fully
// disconnected. These cases wire the real BridgeRegistry to the real
// orchestrator and let a bundle report ACTIVE the way production does.

namespace {

// A host whose spawned bundle behaves like a real Dart activator: it registers
// over the bridge and then reports ACTIVE, rather than calling the orchestrator
// directly the way FakeHost does.
class BridgeReportingHost final : public ihs::osgi::IBundleHost {
 public:
  BridgeReportingHost(ihs::osgi::BridgeRegistry* registry, bool report_active)
      : registry_(registry), report_active_(report_active) {}

  BundleHandle Spawn(const ihs::osgi::BundleManifest& manifest) override {
    const BundleHandle handle = ++next_handle_;
    // What the Dart side does over dev.osgi/bridge: init, then active.
    registry_->RegisterBundle(manifest.symbolic_name,
                              static_cast<int64_t>(handle));
    if (report_active_) {
      registry_->ReportActive(manifest.symbolic_name);
    }
    return handle;
  }
  bool PinThread(BundleHandle, int) override { return true; }
  void Shutdown(BundleHandle) override {}

 private:
  ihs::osgi::BridgeRegistry* registry_;
  bool report_active_;
  BundleHandle next_handle_{0};
};

ihs::osgi::DartPortApi NoopDartApi() {
  return {[](void*) -> intptr_t { return 0; },
          [](int64_t, int64_t) { return true; }};
}

}  // namespace

// The join, end to end: a bundle reports ACTIVE over the bridge and the
// critical wait is released by it. Before the observer existed this timed out.
TEST(OrchestratorBridgeJoin, ActiveOverTheBridgeReleasesTheCriticalWait) {
  auto api = NoopDartApi();
  ihs::osgi::BridgeRegistry registry(api);
  ASSERT_TRUE(registry.InitializeDartApi(0xDEADBEEF));

  BridgeReportingHost host(&registry, /*report_active=*/true);
  BundleStartupOrchestrator orchestrator(
      host, {Manifest("com.ivi.cluster", Priority::kCritical)}, NoStagger());
  registry.SetLifecycleObserver(&orchestrator);

  const auto outcomes = orchestrator.StartCriticalPhase();
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_EQ(outcomes[0].failure, StartupFailure::kNone)
      << "ACTIVE reported over the bridge did not reach the orchestrator";
  EXPECT_EQ(outcomes[0].state, BundleState::kActive);
}

// The control: same wiring, but the bundle never reports. Confirms the pass
// above comes from the report and not from something else releasing the wait.
TEST(OrchestratorBridgeJoin, WithoutTheReportTheCriticalWaitStillTimesOut) {
  auto api = NoopDartApi();
  ihs::osgi::BridgeRegistry registry(api);
  ASSERT_TRUE(registry.InitializeDartApi(0xDEADBEEF));

  BridgeReportingHost host(&registry, /*report_active=*/false);
  BundleStartupOrchestrator orchestrator(
      host, {Manifest("com.ivi.cluster", Priority::kCritical)}, NoStagger());
  registry.SetLifecycleObserver(&orchestrator);

  const auto outcomes = orchestrator.StartCriticalPhase();
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_EQ(outcomes[0].failure, StartupFailure::kTimedOut);
}

// Detaching the observer must reopen the gap rather than leave a stale pointer
// delivering into a destroyed orchestrator.
TEST(OrchestratorBridgeJoin, DetachedObserverStopsReleasingTheWait) {
  auto api = NoopDartApi();
  ihs::osgi::BridgeRegistry registry(api);
  ASSERT_TRUE(registry.InitializeDartApi(0xDEADBEEF));

  BridgeReportingHost host(&registry, /*report_active=*/true);
  BundleStartupOrchestrator orchestrator(
      host, {Manifest("com.ivi.cluster", Priority::kCritical)}, NoStagger());
  registry.SetLifecycleObserver(&orchestrator);
  registry.SetLifecycleObserver(nullptr);

  const auto outcomes = orchestrator.StartCriticalPhase();
  EXPECT_EQ(outcomes[0].failure, StartupFailure::kTimedOut);
}

// --- Critical phase ---------------------------------------------------------

TEST(Orchestrator, CriticalBundlesStartInPlanOrderAndReachActive) {
  FakeHost host;
  BundleStartupOrchestrator orchestrator(
      host,
      {Manifest("navigation", Priority::kNormal),
       Manifest("cluster", Priority::kCritical),
       Manifest("alarms", Priority::kCritical)},
      NoStagger());
  host.Attach(&orchestrator);

  const auto outcomes = orchestrator.StartCriticalPhase();

  ASSERT_EQ(outcomes.size(), 2u) << "only critical bundles are in this phase";
  EXPECT_EQ(outcomes[0].symbolic_name, "cluster");
  EXPECT_EQ(outcomes[1].symbolic_name, "alarms");
  for (const auto& outcome : outcomes) {
    EXPECT_TRUE(outcome.ok()) << outcome.symbolic_name;
    EXPECT_EQ(outcome.state, BundleState::kActive);
  }
  // The normal bundle must not have been touched by this phase.
  EXPECT_EQ(host.spawned(), (std::vector<std::string>{"cluster", "alarms"}));
  EXPECT_EQ(orchestrator.StateOf("navigation"), BundleState::kInstalled);
}

// The case a real engine will not reproduce on demand, and the reason the seam
// exists: an activator that never reports ACTIVE.
TEST(Orchestrator, CriticalBundleThatNeverReportsActiveTimesOut) {
  FakeHost host;
  host.SetAutoActive(false);
  BundleStartupOrchestrator orchestrator(
      host, {Manifest("cluster", Priority::kCritical)}, NoStagger());
  host.Attach(&orchestrator);

  const auto start = std::chrono::steady_clock::now();
  const auto outcomes = orchestrator.StartCriticalPhase();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_EQ(outcomes[0].failure, StartupFailure::kTimedOut);
  EXPECT_GE(elapsed, milliseconds{kShortTimeoutMs})
      << "must actually wait out the deadline";

  // Torn down rather than left holding a view and a vsync registration.
  EXPECT_EQ(host.shutdown_count(), 1u);
  EXPECT_EQ(orchestrator.StateOf("cluster"), BundleState::kResolved)
      << "unwound through STOPPING, so it can be started again";
  EXPECT_TRUE(orchestrator.LiveBundles().empty());
}

// One broken critical bundle must not take the rest of startup with it.
TEST(Orchestrator, StartupContinuesAfterACriticalFailure) {
  FakeHost host;
  host.FailSpawnOf("cluster");
  BundleStartupOrchestrator orchestrator(
      host,
      {Manifest("cluster", Priority::kCritical),
       Manifest("alarms", Priority::kCritical)},
      NoStagger());
  host.Attach(&orchestrator);

  const auto outcomes = orchestrator.StartCriticalPhase();

  ASSERT_EQ(outcomes.size(), 2u);
  EXPECT_EQ(outcomes[0].failure, StartupFailure::kSpawnFailed);
  EXPECT_TRUE(outcomes[1].ok()) << "the next critical bundle still starts";
  EXPECT_EQ(outcomes[1].state, BundleState::kActive);
}

// A spawn failure must not leave a handle or a live state behind.
TEST(Orchestrator, SpawnFailureLeavesNoHandleAndNoLiveState) {
  FakeHost host;
  host.FailSpawnOf("cluster");
  BundleStartupOrchestrator orchestrator(
      host, {Manifest("cluster", Priority::kCritical)}, NoStagger());
  host.Attach(&orchestrator);

  const auto outcomes = orchestrator.StartCriticalPhase();
  EXPECT_EQ(outcomes[0].failure, StartupFailure::kSpawnFailed);
  EXPECT_EQ(orchestrator.HandleOf("cluster"), kInvalidBundleHandle);
  EXPECT_TRUE(orchestrator.LiveBundles().empty());
  EXPECT_EQ(host.shutdown_count(), 0u) << "nothing was created to tear down";
}

// A bundle that dies during startup must not make the phase sit out the full
// deadline waiting for a signal that will never come.
TEST(Orchestrator, StoppedDuringStartupWakesTheWaitEarly) {
  FakeHost host;
  host.SetAutoActive(false);
  BundleStartupOrchestrator orchestrator(
      host, {Manifest("cluster", Priority::kCritical, -1, 5000)}, NoStagger());
  host.Attach(&orchestrator);

  std::thread killer([&] {
    std::this_thread::sleep_for(milliseconds{10});
    orchestrator.NotifyStopped("cluster");
  });

  const auto start = std::chrono::steady_clock::now();
  const auto outcomes = orchestrator.StartCriticalPhase();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  killer.join();

  EXPECT_LT(elapsed, milliseconds{2000})
      << "woke on the stop rather than burning the 5s deadline";
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_FALSE(outcomes[0].ok());
}

// --- CPU affinity -----------------------------------------------------------

TEST(Orchestrator, PinsOnlyBundlesThatAskForACore) {
  FakeHost host;
  BundleStartupOrchestrator orchestrator(
      host,
      {Manifest("cluster", Priority::kCritical, 2),
       Manifest("alarms", Priority::kCritical, -1)},
      NoStagger());
  host.Attach(&orchestrator);

  const auto outcomes = orchestrator.StartCriticalPhase();
  for (const auto& outcome : outcomes) {
    EXPECT_TRUE(outcome.ok()) << outcome.symbolic_name;
  }
  ASSERT_EQ(host.pinned().size(), 1u) << "-1 means do not pin at all";
  EXPECT_EQ(host.pinned()[0].second, 2);
}

// cpu_core was written to buy a scheduling guarantee. Running on without it
// would silently withdraw that guarantee while the bundle still looked healthy,
// so a refusal is fatal to the bundle.
TEST(Orchestrator, RefusedAffinityFailsTheBundleAndTearsItDown) {
  FakeHost host;
  host.RefusePinning();
  BundleStartupOrchestrator orchestrator(
      host, {Manifest("cluster", Priority::kCritical, 2)}, NoStagger());
  host.Attach(&orchestrator);

  const auto outcomes = orchestrator.StartCriticalPhase();
  ASSERT_EQ(outcomes.size(), 1u);
  EXPECT_EQ(outcomes[0].failure, StartupFailure::kAffinityRefused);
  EXPECT_EQ(host.shutdown_count(), 1u);
  EXPECT_TRUE(orchestrator.LiveBundles().empty());
}

// --- Deferred phases --------------------------------------------------------

TEST(Orchestrator, DeferredPhasesLaunchNormalThenBackground) {
  FakeHost host;
  BundleStartupOrchestrator orchestrator(
      host,
      {Manifest("telemetry", Priority::kBackground),
       Manifest("navigation", Priority::kNormal),
       Manifest("media", Priority::kNormal)},
      NoStagger());
  host.Attach(&orchestrator);

  const auto outcomes = orchestrator.StartDeferredPhases();
  ASSERT_EQ(outcomes.size(), 3u);
  EXPECT_EQ(host.spawned(),
            (std::vector<std::string>{"navigation", "media", "telemetry"}));
}

// Nothing waits on a deferred bundle, so one that never reports ACTIVE must not
// stall the phase -- it simply stays STARTING.
TEST(Orchestrator, DeferredPhasesDoNotWaitForActive) {
  FakeHost host;
  host.SetAutoActive(false);
  BundleStartupOrchestrator orchestrator(
      host,
      {Manifest("navigation", Priority::kNormal, -1, 5000),
       Manifest("telemetry", Priority::kBackground, -1, 5000)},
      NoStagger());
  host.Attach(&orchestrator);

  const auto start = std::chrono::steady_clock::now();
  const auto outcomes = orchestrator.StartDeferredPhases();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(elapsed, milliseconds{1000}) << "must not block on a deadline";
  ASSERT_EQ(outcomes.size(), 2u);
  for (const auto& outcome : outcomes) {
    EXPECT_TRUE(outcome.ok());
    EXPECT_EQ(outcome.state, BundleState::kStarting);
  }
}

TEST(Orchestrator, StaggerDelaysNormalBundles) {
  FakeHost host;
  ihs::osgi::StartupPolicy policy;
  policy.normal_stagger = milliseconds{25};
  BundleStartupOrchestrator orchestrator(
      host,
      {Manifest("a", Priority::kNormal), Manifest("b", Priority::kNormal),
       Manifest("c", Priority::kNormal)},
      policy);
  host.Attach(&orchestrator);

  const auto start = std::chrono::steady_clock::now();
  orchestrator.StartDeferredPhases();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  // Third bundle waits 2 gaps.
  EXPECT_GE(elapsed, milliseconds{50});
}

// --- Lifecycle reporting ----------------------------------------------------

TEST(Orchestrator, RejectsActiveFromAnUnknownOrWrongStateBundle) {
  FakeHost host;
  host.SetAutoActive(false);
  BundleStartupOrchestrator orchestrator(
      host, {Manifest("cluster", Priority::kNormal)}, NoStagger());
  host.Attach(&orchestrator);

  // Never spawned, so still INSTALLED: an ACTIVE report is bogus.
  orchestrator.NotifyActive("cluster");
  EXPECT_EQ(orchestrator.StateOf("cluster"), BundleState::kInstalled);

  orchestrator.NotifyActive("does.not.exist");
  EXPECT_EQ(orchestrator.StateOf("does.not.exist"), BundleState::kUninstalled);
}

TEST(Orchestrator, DuplicateActiveReportIsIgnored) {
  FakeHost host;
  BundleStartupOrchestrator orchestrator(
      host, {Manifest("cluster", Priority::kCritical)}, NoStagger());
  host.Attach(&orchestrator);
  ASSERT_TRUE(orchestrator.StartCriticalPhase()[0].ok());
  ASSERT_EQ(orchestrator.StateOf("cluster"), BundleState::kActive);

  orchestrator.NotifyActive("cluster");
  EXPECT_EQ(orchestrator.StateOf("cluster"), BundleState::kActive);
}

// Stop then start again: the state machine's STOPPING -> RESOLVED edge is what
// makes this a normal operation rather than a teardown.
TEST(Orchestrator, BundleCanBeRestartedAfterStopping) {
  FakeHost host;
  BundleStartupOrchestrator orchestrator(
      host, {Manifest("cluster", Priority::kNormal)}, NoStagger());
  host.Attach(&orchestrator);

  ASSERT_TRUE(orchestrator.StartDeferredPhases()[0].ok());
  EXPECT_EQ(orchestrator.StateOf("cluster"), BundleState::kActive);

  orchestrator.NotifyStopped("cluster");
  EXPECT_EQ(orchestrator.StateOf("cluster"), BundleState::kResolved);
  EXPECT_EQ(orchestrator.HandleOf("cluster"), kInvalidBundleHandle);
  EXPECT_TRUE(orchestrator.LiveBundles().empty());

  const auto again = orchestrator.StartDeferredPhases();
  ASSERT_EQ(again.size(), 1u);
  EXPECT_TRUE(again[0].ok());
  EXPECT_EQ(orchestrator.StateOf("cluster"), BundleState::kActive);
  EXPECT_EQ(host.spawned().size(), 2u);
}

TEST(Orchestrator, EmptyConfigDoesNothing) {
  FakeHost host;
  BundleStartupOrchestrator orchestrator(host, {}, NoStagger());
  EXPECT_TRUE(orchestrator.StartCriticalPhase().empty());
  EXPECT_TRUE(orchestrator.StartDeferredPhases().empty());
  EXPECT_TRUE(host.spawned().empty());
}

// The bridge calls NotifyActive from the platform thread while the orchestrator
// waits on the startup thread. The report may land before the wait begins,
// during it, or before Spawn has even returned -- all three must succeed.
//
// A lost signal here shows up as a full-deadline timeout on a bundle that
// actually started, which is why the deadline is generous: a pass has to mean
// the signal arrived, not that the deadline was short enough to hide it.
TEST(Orchestrator, ActiveReportedFromAnotherThreadIsNotLost) {
  for (int attempt = 0; attempt < 10; ++attempt) {
    FakeHost host;
    host.SetAutoActive(false);
    host.SetAsyncActive(true);
    BundleStartupOrchestrator orchestrator(
        host, {Manifest("cluster", Priority::kCritical, -1, 2000)},
        NoStagger());
    host.Attach(&orchestrator);

    const auto outcomes = orchestrator.StartCriticalPhase();
    host.JoinReporters();  // before `orchestrator` leaves scope -- see above

    ASSERT_EQ(outcomes.size(), 1u) << "attempt " << attempt;
    EXPECT_EQ(outcomes[0].failure, StartupFailure::kNone)
        << "attempt " << attempt << ": ACTIVE report was lost";
    EXPECT_EQ(outcomes[0].state, BundleState::kActive) << "attempt " << attempt;
  }
}
