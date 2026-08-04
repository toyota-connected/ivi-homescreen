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

// Parking frame production, which is done by NOT returning the vsync baton.
// Flutter asks for a frame and gets no answer, so it builds none and the whole
// pipeline goes quiet -- the same mechanism the provider already relies on
// during bring-up, where a baton that arrives before the task runner is wired
// is left parked until SetRunner drains it.
//
// No engine or task runner is wired here: PostOnVsync no-ops without them, so
// what the assertions read is whether the baton was taken, which is exactly
// the question. HasParkedBaton() is true while the engine is still waiting.

#include <gtest/gtest.h>

#include "task_runner.h"
#include "vsync/ivsync_provider.h"

namespace {

constexpr intptr_t kBaton = 0x1234;

// A real TaskRunner, so the runner-wired paths are exercised. The engine stays
// null: PostOnVsync bails on a null engine before posting anything, which is
// what keeps this free of a live Flutter engine while still letting the
// provider believe it can deliver. What the assertions then read is whether
// the baton was taken out of vsync_baton_ -- exactly the question.
class WiredRunner {
 public:
  WiredRunner() : runner_("vsync-park-test", engine_) {}
  TaskRunner* get() { return &runner_; }

 private:
  FLUTTER_API_SYMBOL(FlutterEngine) engine_ { nullptr };
  TaskRunner runner_;
};

}  // namespace

TEST(VsyncPark, ParkedDeliveryLeavesTheEngineWaiting) {
  ivi::IVsyncProvider vsync;  // no runner: the baton stays put on submit
  vsync.SubmitBaton(nullptr, kBaton);
  ASSERT_TRUE(vsync.HasParkedBaton()) << "the engine asked for a frame";

  vsync.SetParked(true);
  EXPECT_TRUE(vsync.IsParked());

  // A source event that would normally hand the baton back.
  vsync.DeliverVsync(1000);
  EXPECT_TRUE(vsync.HasParkedBaton())
      << "parked: the baton must still be held, which is what stops frames";

  // A discarded frame must not hand it back either -- that path exists to keep
  // Flutter scheduling, which is the opposite of what parking wants.
  vsync.DeliverDiscard();
  EXPECT_TRUE(vsync.HasParkedBaton());
}

TEST(VsyncPark, UnparkingHandsTheBatonBack) {
  WiredRunner runner;
  ivi::IVsyncProvider vsync;
  vsync.SetEngine(nullptr, runner.get());
  // Park first: an unparked provider with a wired runner delivers the baton
  // inline the moment it is submitted, which is the behavior being gated.
  vsync.SetParked(true);
  vsync.SubmitBaton(nullptr, kBaton);
  vsync.DeliverVsync(1000);
  ASSERT_TRUE(vsync.HasParkedBaton());

  vsync.SetParked(false);
  EXPECT_FALSE(vsync.IsParked());
  EXPECT_FALSE(vsync.HasParkedBaton())
      << "unparking releases the baton the engine was waiting on";
}

// The path that actually carries most of the traffic: the engine asking for a
// frame while the view is parked. SubmitBaton kicks the baton out inline when
// no present is in flight, so a gate that only covers the source event lets
// frame production continue as if nothing were parked.
TEST(VsyncPark, AFreshRequestWhileParkedIsNotServed) {
  WiredRunner runner;
  ivi::IVsyncProvider vsync;
  vsync.SetEngine(nullptr, runner.get());

  vsync.SetParked(true);
  vsync.SubmitBaton(nullptr, kBaton);
  EXPECT_TRUE(vsync.HasParkedBaton())
      << "parked: an inline kick must not deliver either";

  vsync.SetParked(false);
  EXPECT_FALSE(vsync.HasParkedBaton()) << "and unparking releases it";
}

// Losing a baton is worse than holding one: Flutter waits forever for a vsync
// that can never arrive. Without a runner there is nowhere to post, so the
// baton must stay parked rather than be exchanged out and dropped.
TEST(VsyncPark, UnparkingWithNoRunnerKeepsTheBaton) {
  ivi::IVsyncProvider vsync;  // no runner wired
  vsync.SubmitBaton(nullptr, kBaton);
  vsync.SetParked(true);
  ASSERT_TRUE(vsync.HasParkedBaton());

  vsync.SetParked(false);
  EXPECT_TRUE(vsync.HasParkedBaton())
      << "nowhere to deliver: hold it for the kick latch, do not drop it";
}

TEST(VsyncPark, ParkingIsIdempotent) {
  WiredRunner runner;
  ivi::IVsyncProvider vsync;
  vsync.SetEngine(nullptr, runner.get());

  vsync.SetParked(true);
  vsync.SetParked(true);  // a burst of output events must not change anything
  vsync.SubmitBaton(nullptr, kBaton);
  EXPECT_TRUE(vsync.HasParkedBaton());

  vsync.SetParked(false);
  EXPECT_FALSE(vsync.HasParkedBaton());
  // Unparking an unparked provider is a no-op, not a second delivery.
  vsync.SetParked(false);
  EXPECT_FALSE(vsync.IsParked());
}

TEST(VsyncPark, ParkingAnIdlePipelineHoldsNothing) {
  // Nothing outstanding: the engine was not asking for frames when the display
  // went away, so there is no baton to withhold and none to return later.
  ivi::IVsyncProvider vsync;
  vsync.SetParked(true);
  EXPECT_FALSE(vsync.HasParkedBaton());
  vsync.SetParked(false);
  EXPECT_FALSE(vsync.HasParkedBaton());
}
