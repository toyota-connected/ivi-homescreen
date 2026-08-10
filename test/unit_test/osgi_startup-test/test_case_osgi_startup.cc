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
#include <string>
#include <vector>

#include "osgi/bundle_state.h"
#include "osgi/startup_plan.h"

namespace {

using ihs::osgi::BundleState;
using ihs::osgi::BundleStateMachine;
using ihs::osgi::Priority;
using std::chrono::milliseconds;

ihs::osgi::BundleManifest Manifest(const std::string& name,
                                   const Priority priority,
                                   const int timeout_ms = 500,
                                   const int cpu_core = -1) {
  ihs::osgi::BundleManifest manifest;
  manifest.symbolic_name = name;
  manifest.priority = priority;
  manifest.startup_timeout_ms = timeout_ms;
  manifest.cpu_core = cpu_core;
  manifest.config.view.bundle_path = "/bundles/" + name;
  return manifest;
}

// Every state, for exhaustive edge checks.
constexpr BundleState kAllStates[] = {
    BundleState::kUninstalled, BundleState::kInstalled, BundleState::kResolved,
    BundleState::kStarting,    BundleState::kStopping,  BundleState::kActive,
};

}  // namespace

// --- Lifecycle --------------------------------------------------------------

TEST(BundleStateMachine, StartsInstalled) {
  const BundleStateMachine machine("com.ivi.cluster");
  EXPECT_EQ(machine.state(), BundleState::kInstalled);
  EXPECT_EQ(machine.symbolic_name(), "com.ivi.cluster");
  EXPECT_FALSE(machine.IsTerminal());
  EXPECT_FALSE(machine.IsLive());
}

TEST(BundleStateMachine, WalksTheHappyPath) {
  BundleStateMachine machine("com.ivi.cluster");
  ASSERT_TRUE(machine.Transition(BundleState::kResolved));
  ASSERT_TRUE(machine.Transition(BundleState::kStarting));
  EXPECT_TRUE(machine.IsLive()) << "STARTING already holds registrations";
  ASSERT_TRUE(machine.Transition(BundleState::kActive));
  EXPECT_TRUE(machine.IsLive());
  ASSERT_TRUE(machine.Transition(BundleState::kStopping));
  EXPECT_FALSE(machine.IsLive());
  ASSERT_TRUE(machine.Transition(BundleState::kResolved));
  EXPECT_FALSE(machine.IsTerminal());
}

// The edge that makes restart-with-backoff possible: stopping returns to
// RESOLVED, from which the bundle can start again.
TEST(BundleStateMachine, RestartsAfterStopping) {
  BundleStateMachine machine("com.ivi.cluster");
  ASSERT_TRUE(machine.Transition(BundleState::kResolved));
  for (int restart = 0; restart < 3; ++restart) {
    ASSERT_TRUE(machine.Transition(BundleState::kStarting)) << restart;
    ASSERT_TRUE(machine.Transition(BundleState::kActive)) << restart;
    ASSERT_TRUE(machine.Transition(BundleState::kStopping)) << restart;
    ASSERT_TRUE(machine.Transition(BundleState::kResolved)) << restart;
  }
  EXPECT_EQ(machine.state(), BundleState::kResolved);
}

// A failed start (activator throws, or a critical bundle misses its deadline)
// unwinds through STOPPING like a healthy stop, rather than needing its own
// path.
TEST(BundleStateMachine, FailedStartUnwindsThroughStopping) {
  BundleStateMachine machine("com.ivi.cluster");
  ASSERT_TRUE(machine.Transition(BundleState::kResolved));
  ASSERT_TRUE(machine.Transition(BundleState::kStarting));
  ASSERT_TRUE(machine.Transition(BundleState::kStopping))
      << "STARTING -> STOPPING is the failure exit";
  ASSERT_TRUE(machine.Transition(BundleState::kResolved));
}

TEST(BundleStateMachine, RejectsIllegalTransitions) {
  BundleStateMachine machine("com.ivi.cluster");
  // Cannot skip RESOLVED.
  EXPECT_FALSE(machine.Transition(BundleState::kActive));
  EXPECT_FALSE(machine.Transition(BundleState::kStarting));
  EXPECT_EQ(machine.state(), BundleState::kInstalled)
      << "a rejected transition must not move the bundle";

  ASSERT_TRUE(machine.Transition(BundleState::kResolved));
  // Cannot jump straight to ACTIVE without STARTING.
  EXPECT_FALSE(machine.Transition(BundleState::kActive));
  EXPECT_EQ(machine.state(), BundleState::kResolved);
}

// Re-asserting the current state is not an edge: a caller doing that has lost
// track of where the bundle is, and accepting it would hide the bug.
TEST(BundleStateMachine, SelfTransitionIsRejected) {
  for (const BundleState state : kAllStates) {
    EXPECT_FALSE(ihs::osgi::IsLegalTransition(state, state))
        << BundleStateName(state);
  }
}

TEST(BundleStateMachine, UninstalledIsTerminal) {
  BundleStateMachine machine("com.ivi.cluster");
  ASSERT_TRUE(machine.Transition(BundleState::kUninstalled));
  EXPECT_TRUE(machine.IsTerminal());
  for (const BundleState state : kAllStates) {
    EXPECT_FALSE(ihs::osgi::IsLegalTransition(BundleState::kUninstalled, state))
        << "no edge out of UNINSTALLED, tried " << BundleStateName(state);
  }
}

// A running bundle cannot be uninstalled out from under itself; it has to stop
// first. Otherwise the bridge and coordinator registrations would outlive it.
TEST(BundleStateMachine, CannotUninstallWhileLive) {
  EXPECT_FALSE(ihs::osgi::IsLegalTransition(BundleState::kStarting,
                                            BundleState::kUninstalled));
  EXPECT_FALSE(ihs::osgi::IsLegalTransition(BundleState::kActive,
                                            BundleState::kUninstalled));
  EXPECT_FALSE(ihs::osgi::IsLegalTransition(BundleState::kStopping,
                                            BundleState::kUninstalled));
}

TEST(BundleStateMachine, StateNamesAreDistinct) {
  for (const BundleState a : kAllStates) {
    EXPECT_FALSE(BundleStateName(a).empty());
    for (const BundleState b : kAllStates) {
      if (a != b) {
        EXPECT_NE(BundleStateName(a), BundleStateName(b));
      }
    }
  }
}

// --- Startup plan -----------------------------------------------------------

TEST(StartupPlan, EmptyInputYieldsEmptyPlan) {
  const auto plan = ihs::osgi::BuildStartupPlan({});
  EXPECT_TRUE(plan.empty());
  EXPECT_EQ(plan.CriticalBudget(), milliseconds{0});
  EXPECT_EQ(plan.NormalSpan(), milliseconds{0});
  EXPECT_TRUE(plan.LaunchOrder().empty());
}

TEST(StartupPlan, SplitsByPriority) {
  const auto plan = ihs::osgi::BuildStartupPlan({
      Manifest("navigation", Priority::kNormal),
      Manifest("cluster", Priority::kCritical),
      Manifest("telemetry", Priority::kBackground),
      Manifest("alarms", Priority::kCritical),
  });

  ASSERT_EQ(plan.critical.size(), 2u);
  ASSERT_EQ(plan.normal.size(), 1u);
  ASSERT_EQ(plan.background.size(), 1u);
  EXPECT_EQ(plan.size(), 4u);

  // Declaration order is preserved within a phase: the config file is the
  // startup script, and it is the only ordering an integrator controls.
  EXPECT_EQ(plan.critical[0].symbolic_name, "cluster");
  EXPECT_EQ(plan.critical[1].symbolic_name, "alarms");
  EXPECT_EQ(plan.LaunchOrder(),
            (std::vector<std::string>{"cluster", "alarms", "navigation",
                                      "telemetry"}));
}

TEST(StartupPlan, CarriesManifestIndexAndAffinity) {
  const auto plan = ihs::osgi::BuildStartupPlan({
      Manifest("navigation", Priority::kNormal, 500, -1),
      Manifest("cluster", Priority::kCritical, 750, 3),
  });
  ASSERT_EQ(plan.critical.size(), 1u);
  // The index must point back at the original manifest, not at the position
  // within the phase -- the orchestrator resolves the full Config through it.
  EXPECT_EQ(plan.critical[0].manifest_index, 1u);
  EXPECT_EQ(plan.critical[0].cpu_core, 3);
  ASSERT_EQ(plan.normal.size(), 1u);
  EXPECT_EQ(plan.normal[0].manifest_index, 0u);
  EXPECT_EQ(plan.normal[0].cpu_core, -1);
}

TEST(StartupPlan, StaggersNormalBundlesByPositionInPhase) {
  const auto plan = ihs::osgi::BuildStartupPlan({
      Manifest("a", Priority::kNormal),
      Manifest("cluster", Priority::kCritical),
      Manifest("b", Priority::kNormal),
      Manifest("c", Priority::kNormal),
  });

  ASSERT_EQ(plan.normal.size(), 3u);
  // Offsets count position within the normal phase, not position in the config
  // -- the interleaved critical entry must not consume a stagger slot.
  EXPECT_EQ(plan.normal[0].start_delay, milliseconds{0});
  EXPECT_EQ(plan.normal[1].start_delay, milliseconds{50});
  EXPECT_EQ(plan.normal[2].start_delay, milliseconds{100});
  EXPECT_EQ(plan.NormalSpan(), milliseconds{100});
}

TEST(StartupPlan, HonorsACustomStagger) {
  ihs::osgi::StartupPolicy policy;
  policy.normal_stagger = milliseconds{200};
  const auto plan = ihs::osgi::BuildStartupPlan(
      {
          Manifest("a", Priority::kNormal),
          Manifest("b", Priority::kNormal),
      },
      policy);
  EXPECT_EQ(plan.normal[1].start_delay, milliseconds{200});
  EXPECT_EQ(plan.NormalSpan(), milliseconds{200});
}

TEST(StartupPlan, ZeroStaggerLaunchesNormalBundlesTogether) {
  ihs::osgi::StartupPolicy policy;
  policy.normal_stagger = milliseconds{0};
  const auto plan = ihs::osgi::BuildStartupPlan(
      {
          Manifest("a", Priority::kNormal),
          Manifest("b", Priority::kNormal),
      },
      policy);
  EXPECT_EQ(plan.normal[0].start_delay, milliseconds{0});
  EXPECT_EQ(plan.normal[1].start_delay, milliseconds{0});
  EXPECT_EQ(plan.NormalSpan(), milliseconds{0});
}

// Only critical bundles are awaited, so only they carry a deadline.
TEST(StartupPlan, OnlyCriticalBundlesCarryATimeout) {
  const auto plan = ihs::osgi::BuildStartupPlan({
      Manifest("cluster", Priority::kCritical, 750),
      Manifest("navigation", Priority::kNormal, 750),
      Manifest("telemetry", Priority::kBackground, 750),
  });
  EXPECT_EQ(plan.critical[0].startup_timeout, milliseconds{750});
  EXPECT_EQ(plan.normal[0].startup_timeout, milliseconds{0});
  EXPECT_EQ(plan.background[0].startup_timeout, milliseconds{0});
}

// The number that decides whether a config is viable: critical bundles are
// awaited one after another, so their timeouts add up to the worst case the
// reactor is blocked before anything is serviced.
TEST(StartupPlan, CriticalBudgetSumsSequentialTimeouts) {
  const auto plan = ihs::osgi::BuildStartupPlan({
      Manifest("cluster", Priority::kCritical, 500),
      Manifest("alarms", Priority::kCritical, 750),
      Manifest("dmi", Priority::kCritical, 250),
      Manifest("navigation", Priority::kNormal, 9999),
  });
  EXPECT_EQ(plan.CriticalBudget(), milliseconds{1500})
      << "normal bundles must not contribute: nothing waits on them";
}

TEST(StartupPlan, AllOnePriorityDegradesCleanly) {
  const auto plan = ihs::osgi::BuildStartupPlan({
      Manifest("a", Priority::kBackground),
      Manifest("b", Priority::kBackground),
  });
  EXPECT_TRUE(plan.critical.empty());
  EXPECT_TRUE(plan.normal.empty());
  EXPECT_EQ(plan.background.size(), 2u);
  EXPECT_EQ(plan.CriticalBudget(), milliseconds{0})
      << "nothing blocks the reactor when no bundle is critical";
  EXPECT_EQ(plan.LaunchOrder(), (std::vector<std::string>{"a", "b"}));
}
