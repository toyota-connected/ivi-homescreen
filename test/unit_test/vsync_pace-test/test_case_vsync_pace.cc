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

// Pacing vsync to the display: at most one baton per refresh, a second request
// in the same refresh deferred to the next vblank, an idle display still woken
// at once.
//
// As in the park tests, no engine is wired, so HasParkedBaton() reads whether
// the baton was handed out. The clock and the deferred delivery go through the
// provider's test seam: the test sets the time, sees what was scheduled and
// fires it.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <ctime>
#include <thread>
#include <vector>

#include "task_runner.h"
#include "vsync/ivsync_provider.h"

namespace {

constexpr intptr_t kBaton = 0x1234;
constexpr uint64_t kPeriod = 16'666'667;
constexpr uint64_t kVblank = 1'000'000'000;  // a presentation at 1 s
constexpr uint64_t kMs = 1'000'000;

class WiredRunner {
 public:
  WiredRunner() : runner_("vsync-pace-test", engine_) {}
  TaskRunner* get() { return &runner_; }

 private:
  FLUTTER_API_SYMBOL(FlutterEngine) engine_ { nullptr };
  TaskRunner runner_;
};

class VsyncPace : public ::testing::Test {
 protected:
  void SetUp() override {
    vsync_.SetEngine(nullptr, runner_.get());
    vsync_.SetPeriodNs(static_cast<uint32_t>(kPeriod));
    vsync_.SetPaceToVblank(true);
    vsync_.SetPacingClockForTest(
        [this] { return now_; },
        [this](uint64_t due) { scheduled_.push_back(due); });
  }

  // A presentation at `at`: the flip hands back whatever baton is held.
  void Present(uint64_t at) {
    now_ = at;
    vsync_.DeliverVsync(at);
  }

  // The engine asks for a frame at `at`; true when it was answered at once.
  bool Request(uint64_t at) {
    now_ = at;
    vsync_.SubmitBaton(nullptr, kBaton);
    return !vsync_.HasParkedBaton();
  }

  WiredRunner runner_;
  ivi::IVsyncProvider vsync_;
  uint64_t now_{0};
  std::vector<uint64_t> scheduled_;
};

}  // namespace

TEST_F(VsyncPace, OffAnswersEveryRequestAtOnce) {
  vsync_.SetPaceToVblank(false);
  Present(kVblank);
  EXPECT_TRUE(Request(kVblank + 1 * kMs));
  EXPECT_TRUE(Request(kVblank + 2 * kMs));
  EXPECT_TRUE(Request(kVblank + 3 * kMs));
  EXPECT_TRUE(scheduled_.empty());
}

TEST_F(VsyncPace, ASecondRequestInARefreshWaitsForTheNextVblank) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs)) << "the first is answered at once";

  EXPECT_FALSE(Request(kVblank + 2 * kMs))
      << "a baton already went out this refresh";
  ASSERT_EQ(scheduled_.size(), 1U);
  EXPECT_EQ(scheduled_[0], kVblank + kPeriod) << "due at the next vblank";

  now_ = kVblank + kPeriod;
  vsync_.FireDeferredForTest();
  EXPECT_FALSE(vsync_.HasParkedBaton()) << "handed out on the vblank";
}

TEST_F(VsyncPace, ADeferredFrameStartsOnItsVblank) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 2 * kMs));
  now_ = kVblank + kPeriod + 300'000;  // the timer fires a little late
  vsync_.FireDeferredForTest();
  ASSERT_FALSE(vsync_.HasParkedBaton());

  // Counted from the vblank, not from when the timer ran: the next request in
  // this refresh waits for the one after it.
  EXPECT_FALSE(Request(kVblank + kPeriod + 1 * kMs));
  ASSERT_EQ(scheduled_.size(), 2U);
  EXPECT_EQ(scheduled_[1], kVblank + 2 * kPeriod);
}

TEST_F(VsyncPace, AnIdleDisplayIsWokenAtOnce) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  // Nothing for a few refreshes, then input.
  EXPECT_TRUE(Request(kVblank + 3 * kPeriod + 5 * kMs));
  EXPECT_TRUE(scheduled_.empty());
}

TEST_F(VsyncPace, TheFirstRequestIsAnsweredAtOnce) {
  EXPECT_TRUE(Request(5 * kMs)) << "no presentation yet";
  EXPECT_TRUE(scheduled_.empty());
}

TEST_F(VsyncPace, AFlipBeforeTheTimerDeliversAndDisarmsIt) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  vsync_.SetSourcePending(true);  // that frame is presented
  ASSERT_FALSE(Request(kVblank + 2 * kMs));
  EXPECT_TRUE(scheduled_.empty()) << "a present is in flight: its flip answers";

  vsync_.SetSourcePending(false);
  Present(kVblank + kPeriod);
  EXPECT_FALSE(vsync_.HasParkedBaton()) << "the flip hands it back";

  // A request after that flip in the same refresh is deferred again, and the
  // flip disarmed the earlier delivery so nothing fires twice.
  vsync_.FireDeferredForTest();
  EXPECT_FALSE(Request(kVblank + kPeriod + 2 * kMs));
  ASSERT_EQ(scheduled_.size(), 1U);
  EXPECT_EQ(scheduled_[0], kVblank + 2 * kPeriod);
}

TEST_F(VsyncPace, AFlipWhileDeferredHandsTheBatonBackOnTheFlip) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 2 * kMs));
  ASSERT_EQ(scheduled_.size(), 1U);

  Present(kVblank + kPeriod);
  EXPECT_FALSE(vsync_.HasParkedBaton());

  vsync_.SubmitBaton(nullptr, kBaton);  // the next request, deferred
  ASSERT_TRUE(vsync_.HasParkedBaton());
  // The timer armed for the first deferral must not answer this one: it is
  // due at a vblank the flip already served.
  EXPECT_EQ(scheduled_.back(), kVblank + 2 * kPeriod);
}

TEST_F(VsyncPace, APresentInFlightAtTheVblankLeavesItToTheFlip) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 2 * kMs));

  vsync_.SetSourcePending(true);  // a present went out before the timer ran
  now_ = kVblank + kPeriod;
  vsync_.FireDeferredForTest();
  EXPECT_TRUE(vsync_.HasParkedBaton()) << "its flip returns the baton";

  vsync_.SetSourcePending(false);
  Present(kVblank + kPeriod + 1 * kMs);
  EXPECT_FALSE(vsync_.HasParkedBaton());
}

TEST_F(VsyncPace, ParkingHoldsADeferredBaton) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 2 * kMs));

  vsync_.SetParked(true);
  now_ = kVblank + kPeriod;
  vsync_.FireDeferredForTest();
  EXPECT_TRUE(vsync_.HasParkedBaton()) << "parked: the timer must not deliver";

  vsync_.SetParked(false);
  EXPECT_FALSE(vsync_.HasParkedBaton());
}

TEST_F(VsyncPace, RepeatedRequestsArmOneDelivery) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 2 * kMs));
  ASSERT_FALSE(Request(kVblank + 3 * kMs));
  EXPECT_EQ(scheduled_.size(), 1U);
}

TEST_F(VsyncPace, StoppingDropsADeferredDelivery) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 2 * kMs));
  vsync_.Stop();
  vsync_.FireDeferredForTest();  // must not touch a stopped provider's engine
  SUCCEED();
}

TEST_F(VsyncPace, AFlipHandsBackAtOnceByDefault) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 2 * kMs));
  // A presentation in the same refresh still delivers.
  Present(kVblank + 3 * kMs);
  EXPECT_FALSE(vsync_.HasParkedBaton());
}

TEST_F(VsyncPace, WithPaceFlipsAnOlderFramesPresentationWaits) {
  vsync_.SetPaceFlips(true);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs)) << "the first frame of the refresh";
  ASSERT_FALSE(Request(kVblank + 2 * kMs));
  ASSERT_EQ(scheduled_.size(), 1U);

  // An older frame is shown on this refresh's vblank, after the baton went
  // out: it must not start a second frame.
  Present(kVblank);
  EXPECT_TRUE(vsync_.HasParkedBaton()) << "left for the next vblank";
  ASSERT_FALSE(scheduled_.empty());
  EXPECT_EQ(scheduled_.back(), kVblank + kPeriod);

  now_ = kVblank + kPeriod;
  vsync_.FireDeferredForTest();
  EXPECT_FALSE(vsync_.HasParkedBaton()) << "handed out on the vblank";
}

TEST_F(VsyncPace, WithPaceFlipsANextRefreshPresentationDelivers) {
  vsync_.SetPaceFlips(true);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 2 * kMs));
  Present(kVblank + kPeriod);
  EXPECT_FALSE(vsync_.HasParkedBaton()) << "a new refresh: delivered at once";
}

TEST_F(VsyncPace, WithPaceFlipsAPresentationWithNoBatonArmsNothing) {
  vsync_.SetPaceFlips(true);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  Present(kVblank);  // nothing parked
  EXPECT_TRUE(scheduled_.empty());
}

TEST_F(VsyncPace, KeepPhaseDefersARequestInTheRefreshAfterAHandOut) {
  vsync_.SetPaceKeepPhase(true);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  EXPECT_FALSE(Request(kVblank + kPeriod + 6 * kMs));
  ASSERT_EQ(scheduled_.size(), 1U);
  EXPECT_EQ(scheduled_[0], kVblank + 2 * kPeriod);
}

TEST_F(VsyncPace, WithoutKeepPhaseThatRequestIsAnsweredAtOnce) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  EXPECT_TRUE(Request(kVblank + kPeriod + 6 * kMs));
}

// The real timer, end to end: a deferred baton is handed out by the runner's
// io_context once the vblank passes.
TEST(VsyncPaceTimer, TheRunnerDeliversADeferredBaton) {
  WiredRunner runner;
  ivi::IVsyncProvider vsync;
  vsync.SetEngine(nullptr, runner.get());
  vsync.SetPeriodNs(static_cast<uint32_t>(kPeriod));
  vsync.SetPaceToVblank(true);

  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
                       static_cast<uint64_t>(ts.tv_nsec);
  vsync.DeliverVsync(now);
  vsync.SubmitBaton(nullptr, kBaton);
  ASSERT_FALSE(vsync.HasParkedBaton());
  vsync.SubmitBaton(nullptr, kBaton);
  ASSERT_TRUE(vsync.HasParkedBaton()) << "deferred to the next vblank";

  for (int i = 0; i < 200 && vsync.HasParkedBaton(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(vsync.HasParkedBaton()) << "the timer handed it out";
  vsync.Stop();
}

TEST_F(VsyncPace, WithPaceFlipsADiscardInTheSameRefreshWaits) {
  vsync_.SetPaceFlips(true);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 2 * kMs));
  now_ = kVblank + 3 * kMs;
  vsync_.DeliverDiscard();  // an older commit replaced before it was shown
  EXPECT_TRUE(vsync_.HasParkedBaton()) << "not a second frame this refresh";
  EXPECT_EQ(scheduled_.back(), kVblank + kPeriod);
}

TEST_F(VsyncPace, AStaleDeferredDueDoesNotDeliver) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 2 * kMs));
  Present(kVblank + kPeriod);  // the flip delivers and disarms
  ASSERT_FALSE(vsync_.HasParkedBaton());
  vsync_.FireDeferredForTest();  // the old wait fires late
  EXPECT_FALSE(vsync_.HasParkedBaton());
}

namespace {
uint64_t MonotonicNowNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
         static_cast<uint64_t>(ts.tv_nsec);
}

// Defers one baton to the next vblank and waits for the runner to hand it out.
bool DeferredBatonIsDelivered(ivi::IVsyncProvider& vsync) {
  vsync.DeliverVsync(MonotonicNowNs());
  vsync.SubmitBaton(nullptr, kBaton);
  if (vsync.HasParkedBaton()) {
    return false;  // the first request must be answered at once
  }
  vsync.SubmitBaton(nullptr, kBaton);
  if (!vsync.HasParkedBaton()) {
    return false;  // the second must wait for the vblank
  }
  for (int i = 0; i < 200 && vsync.HasParkedBaton(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return !vsync.HasParkedBaton();
}
}  // namespace

// A Wayland display's provider outlives the view whose runner it was wired to,
// and is wired to the next view's runner.
TEST(VsyncPaceTimer, AStoppedProviderDeliversOnTheNextRunner) {
  ivi::IVsyncProvider vsync;
  vsync.SetPeriodNs(static_cast<uint32_t>(kPeriod));
  vsync.SetPaceToVblank(true);
  {
    WiredRunner first;
    vsync.SetEngine(nullptr, first.get());
    EXPECT_TRUE(DeferredBatonIsDelivered(vsync));
    vsync.Stop();
  }
  WiredRunner second;
  vsync.SetEngine(nullptr, second.get());
  EXPECT_TRUE(DeferredBatonIsDelivered(vsync)) << "the waits reach it again";
  vsync.Stop();
}

TEST(VsyncPaceTimer, AWaitStillArmedWhenTheRunnerGoesIsHarmless) {
  ivi::IVsyncProvider vsync;
  vsync.SetPeriodNs(static_cast<uint32_t>(kPeriod));
  vsync.SetPaceToVblank(true);
  {
    WiredRunner runner;
    vsync.SetEngine(nullptr, runner.get());
    vsync.DeliverVsync(MonotonicNowNs());
    vsync.SubmitBaton(nullptr, kBaton);
    vsync.SubmitBaton(nullptr, kBaton);  // armed for the next vblank
    vsync.Stop();
  }  // the runner and its io_context go first, as on Wayland
  SUCCEED();
}
