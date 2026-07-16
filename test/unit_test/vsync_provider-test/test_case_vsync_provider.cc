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

// Unit tests for ivi::IVsyncProvider state-machine behaviour.
//
// IMPORTANT: These tests deliberately avoid any code path that calls
// LibFlutterEngine (PostOnVsync / DeliverBaton / DrainParkedBaton when
// engine != nullptr).  LibFlutterEngine is not loaded in a unit-test binary
// (no libflutter_engine.so).  Only the composable state accessors and the
// baton-parking logic that runs before SetEngine() is called are exercised.

#include <cstdint>

#include <gtest/gtest.h>

#include "vsync/ivsync_provider.h"

namespace {

// Minimal concrete subclass: delegates IsSourcePending/PeriodNs to the base
// class atomics so SetSourcePending/SetPeriodNs drive them.
// Exposes the protected virtual accessors as public wrappers so TEST
// functions can call them without subclassing inside each test.
class TestVsyncProvider final : public ivi::IVsyncProvider {
 public:
  TestVsyncProvider() = default;

  // Public wrappers for the protected virtual accessors.
  [[nodiscard]] bool SourcePending() const { return IsSourcePending(); }
  [[nodiscard]] uint32_t Period() const { return PeriodNs(); }
};

}  // namespace

// ---- Default state --------------------------------------------------------

TEST(VsyncProvider, DefaultPeriodNsIsNonZero) {
  // The base class initialises period_ns_ to 16'666'667 (≈ 60 Hz).
  TestVsyncProvider p;
  EXPECT_GT(p.Period(), 0u);
}

TEST(VsyncProvider, DefaultNotPending) {
  TestVsyncProvider p;
  EXPECT_FALSE(p.SourcePending());
}

TEST(VsyncProvider, LastFrameStartNsInitiallyZero) {
  TestVsyncProvider p;
  EXPECT_EQ(p.LastDeliveredFrameStartNs(), 0u);
}

// ---- Setters --------------------------------------------------------------

TEST(VsyncProvider, SetPeriodNsStored) {
  TestVsyncProvider p;
  p.SetPeriodNs(33'333'333u);  // ~30 Hz
  EXPECT_EQ(p.Period(), 33'333'333u);
}

TEST(VsyncProvider, SetSourcePendingTrue) {
  TestVsyncProvider p;
  p.SetSourcePending(true);
  EXPECT_TRUE(p.SourcePending());
}

TEST(VsyncProvider, SetSourcePendingFalse) {
  TestVsyncProvider p;
  p.SetSourcePending(true);
  p.SetSourcePending(false);
  EXPECT_FALSE(p.SourcePending());
}

// ---- EnableProfile --------------------------------------------------------

TEST(VsyncProvider, EnableProfileNoCrash) {
  TestVsyncProvider p;
  // Enabled=false: no env var set, profiling off.  Must not crash.
  p.EnableProfile(false, "test-label");
  p.EnableProfile(true, "test-label");
}

// ---- Stop on idle provider (no baton, no engine) -------------------------

TEST(VsyncProvider, StopIdleNoCrash) {
  TestVsyncProvider p;
  p.Stop();  // nothing parked, no engine — must be a no-op
}

// ---- SubmitBaton when pending: baton is parked (engine=nullptr) -----------
//
// With engine == nullptr, DrainParkedBaton() exits early (engine check).
// So a baton submitted while a present is in flight AND no engine is wired
// stays parked. We verify Stop() can then clean it up without crashing.

TEST(VsyncProvider, SubmitBatonParkedWhenPendingAndNoEngine) {
  TestVsyncProvider p;
  p.SetSourcePending(true);

  // Submit a non-zero baton (engine=nullptr simulated by never calling SetEngine).
  // The provider stores it; DrainParkedBaton is a no-op (engine_ == nullptr).
  p.SubmitBaton(nullptr, 42);

  // Stop() with a parked baton must not crash.
  p.Stop();
}

// ---- SubmitBaton when idle (not pending) and engine==nullptr --------------
//
// Pipeline idle: DrainParkedBaton is called, but engine_ == nullptr so it
// returns false.  The baton exchange still clears vsync_baton_.

TEST(VsyncProvider, SubmitBatonIdleNoEngineClearsAtomically) {
  TestVsyncProvider p;
  p.SetSourcePending(false);

  // Exchange runs; returns false (no engine). No crash.
  p.SubmitBaton(nullptr, 99);

  // LastDeliveredFrameStartNs stays 0 because PostOnVsync was never called.
  EXPECT_EQ(p.LastDeliveredFrameStartNs(), 0u);
}

// ---- DeliverParkedBaton when no baton is parked --------------------------

TEST(VsyncProvider, DeliverParkedBatonEmptyReturnsFalse) {
  TestVsyncProvider p;
  // No SubmitBaton called; vsync_baton_ == 0.
  EXPECT_FALSE(p.DeliverParkedBaton());
}

// ---- Double Stop ----------------------------------------------------------

TEST(VsyncProvider, DoubleStopNoCrash) {
  TestVsyncProvider p;
  p.Stop();
  p.Stop();
}
