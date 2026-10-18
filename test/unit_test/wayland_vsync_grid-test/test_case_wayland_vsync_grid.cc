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

// The Wayland provider's grid pacing and latch lead, without a compositor:
// presentations are fed straight into OnPresented, and the clock and deferred
// delivery go through the provider's test seam, as in vsync_pace-test.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "task_runner.h"
#include "vsync/wayland_vsync_provider.h"

namespace {

constexpr intptr_t kBaton = 0x1234;
constexpr uint32_t kPeriod = 16'666'667;
constexpr uint64_t kVblank = 1'000'000'000;
constexpr uint64_t kMs = 1'000'000;

class WiredRunner {
 public:
  WiredRunner() : runner_("wayland-vsync-grid-test", engine_) {}
  TaskRunner* get() { return &runner_; }

 private:
  FLUTTER_API_SYMBOL(FlutterEngine) engine_ { nullptr };
  TaskRunner runner_;
};

class WaylandVsyncGrid : public ::testing::Test {
 protected:
  void SetUp() override {
    vsync_.SetEngine(nullptr, runner_.get());
    vsync_.SetGridPacing(true);
    vsync_.SetPacingClockForTest(
        [this] { return now_; },
        [this](uint64_t due) { scheduled_.push_back(due); });
  }
  void TearDown() override { vsync_.Stop(); }

  // The compositor reports a commit shown at `at`.
  void Present(uint64_t at) {
    now_ = at;
    ivi::WpFeedbackHandler fb(&vsync_);
    vsync_.OnPresented(at, kPeriod, 0, 0, &fb);
  }

  // The engine asks for a frame at `at`; true when it was answered at once.
  bool Request(uint64_t at) {
    now_ = at;
    vsync_.SubmitBaton(nullptr, kBaton);
    return !vsync_.HasParkedBaton();
  }

  WiredRunner runner_;
  ivi::WaylandVsyncProvider vsync_;
  uint64_t now_{0};
  std::vector<uint64_t> scheduled_;
};

}  // namespace

TEST_F(WaylandVsyncGrid, FramesStartOnTheVblanksNotOnEachPresentation) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  // The next request waits for the next vblank, not for that frame's
  // presentation, which may come a refresh late.
  ASSERT_FALSE(Request(kVblank + 3 * kMs));
  ASSERT_EQ(scheduled_.size(), 1U);
  EXPECT_EQ(scheduled_[0], kVblank + kPeriod);
  now_ = kVblank + kPeriod;
  vsync_.FireDeferredForTest();
  EXPECT_FALSE(vsync_.HasParkedBaton()) << "started on the vblank";
}

TEST_F(WaylandVsyncGrid, ALatePresentationDoesNotStartASecondFrame) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 2 * kMs));
  now_ = kVblank + kPeriod;
  vsync_.FireDeferredForTest();
  ASSERT_FALSE(Request(kVblank + kPeriod + 1 * kMs))
      << "a baton went out on this vblank";
  // The frame from the first refresh is shown now, on this refresh's vblank.
  Present(kVblank + kPeriod);
  EXPECT_TRUE(vsync_.HasParkedBaton()) << "one frame per refresh";
  EXPECT_EQ(scheduled_.back(), kVblank + 2 * kPeriod);
}

TEST_F(WaylandVsyncGrid, OffRequestsAreAnsweredAtOnce) {
  vsync_.SetGridPacing(false);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  EXPECT_TRUE(Request(kVblank + 2 * kMs)) << "unpaced, nothing in flight";
}

TEST_F(WaylandVsyncGrid, NoLatchLeadBeforeAPresentation) {
  vsync_.NoteLatch(kVblank);
  EXPECT_EQ(vsync_.LatchLeadNs(), 0U);
}

TEST_F(WaylandVsyncGrid, LatchLeadIsTheTimeFromTheTakeToTheNextVblank) {
  Present(kVblank);
  vsync_.NoteLatch(kVblank + 4 * kMs + 500'000);
  EXPECT_EQ(vsync_.LatchLeadNs(), kPeriod - 4 * kMs - 500'000);
}

TEST_F(WaylandVsyncGrid, LatchLeadCountsTakesRefreshesAfterThePresentation) {
  Present(kVblank);
  vsync_.NoteLatch(kVblank + 2 * kPeriod + 5 * kMs);
  EXPECT_EQ(vsync_.LatchLeadNs(), kPeriod - 5 * kMs);
}

TEST_F(WaylandVsyncGrid, AnotherLayerOfTheSameFrameIsIgnored) {
  Present(kVblank);
  vsync_.NoteLatch(kVblank + 4 * kMs);
  vsync_.NoteLatch(kVblank + 8 * kMs);  // the same frame's second layer
  EXPECT_EQ(vsync_.LatchLeadNs(), kPeriod - 4 * kMs);
}

TEST_F(WaylandVsyncGrid, AnEarlierTakeWidensTheLeadAtOnce) {
  Present(kVblank);
  vsync_.NoteLatch(kVblank + 6 * kMs);
  vsync_.NoteLatch(kVblank + kPeriod + 3 * kMs);
  EXPECT_EQ(vsync_.LatchLeadNs(), kPeriod - 3 * kMs);
}

TEST_F(WaylandVsyncGrid, ALaterTakeNarrowsTheLeadSlowly) {
  Present(kVblank);
  vsync_.NoteLatch(kVblank + 3 * kMs);
  const uint32_t wide = vsync_.LatchLeadNs();
  vsync_.NoteLatch(kVblank + kPeriod + 9 * kMs);
  const uint32_t after_one = vsync_.LatchLeadNs();
  EXPECT_LT(after_one, wide);
  EXPECT_GT(after_one, kPeriod - 9 * kMs) << "one late take is not trusted";
  for (uint64_t i = 2; i < 600; ++i) {
    vsync_.NoteLatch(kVblank + i * kPeriod + 9 * kMs);
  }
  EXPECT_NEAR(static_cast<double>(vsync_.LatchLeadNs()),
              static_cast<double>(kPeriod - 9 * kMs), 0.1 * kMs)
      << "a lasting change is followed";
}

TEST_F(WaylandVsyncGrid, ARequestMidRefreshWhileFramesComeWaitsForTheVblank) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  // A platform view submits mid-refresh in the next refresh: the frame it
  // asks for starts on the vblank after, not now.
  EXPECT_FALSE(Request(kVblank + kPeriod + 8 * kMs));
  ASSERT_EQ(scheduled_.size(), 1U);
  EXPECT_EQ(scheduled_[0], kVblank + 2 * kPeriod);
}

TEST_F(WaylandVsyncGrid, ARequestJustAfterTheVblankStartsItsFrameAtOnce) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  // A platform view submits just after the next vblank, in the start window:
  // a frame started now still makes this refresh.
  EXPECT_TRUE(Request(kVblank + kPeriod + 1 * kMs));
  EXPECT_TRUE(scheduled_.empty());
}

TEST_F(WaylandVsyncGrid, AFullPipelineHoldsARequestInTheStartWindow) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  vsync_.SetInFlightForTest(ivi::WaylandVsyncProvider::kGridInFlight);
  EXPECT_FALSE(Request(kVblank + kPeriod + 1 * kMs))
      << "the window does not start a frame past the in-flight depth";
  EXPECT_TRUE(scheduled_.empty()) << "a presentation returns the baton";
}

TEST_F(WaylandVsyncGrid, AZeroStartWindowKeepsThatRequestForTheVblank) {
  vsync_.SetStartWindow(0);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  EXPECT_FALSE(Request(kVblank + kPeriod + 1 * kMs));
  ASSERT_EQ(scheduled_.size(), 1U);
  EXPECT_EQ(scheduled_[0], kVblank + 2 * kPeriod);
}

TEST_F(WaylandVsyncGrid, AStartWindowSetOffTheGridHoldsOnceItIsOn) {
  vsync_.SetGridPacing(false);
  vsync_.SetStartWindow(0);
  vsync_.SetGridPacing(true);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  EXPECT_FALSE(Request(kVblank + kPeriod + 1 * kMs));
}

TEST_F(WaylandVsyncGrid, ARequestAfterAnIdleRefreshIsAnsweredAtOnce) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  EXPECT_TRUE(Request(kVblank + 2 * kPeriod + 3 * kMs))
      << "a refresh went by with no frame: wake at once";
  EXPECT_TRUE(scheduled_.empty());
}

TEST_F(WaylandVsyncGrid, AFullPipelineHoldsTheBatonForAPresentation) {
  Present(kVblank);
  vsync_.SetInFlightForTest(ivi::WaylandVsyncProvider::kGridInFlight);
  EXPECT_FALSE(Request(kVblank + 1 * kMs));
  EXPECT_TRUE(scheduled_.empty()) << "a presentation returns it, not a timer";
  Present(kVblank + kPeriod);
  EXPECT_FALSE(vsync_.HasParkedBaton());
}

TEST_F(WaylandVsyncGrid, AtDepthOneACommitInFlightHoldsTheBaton) {
  vsync_.SetGridDepth(1);
  Present(kVblank);
  vsync_.SetInFlightForTest(1);
  EXPECT_FALSE(Request(kVblank + 1 * kMs));
  vsync_.SetGridDepth(2);
  vsync_.SetInFlightForTest(1);
  EXPECT_TRUE(Request(kVblank + 2 * kPeriod + 1 * kMs))
      << "at depth 2 one commit in flight does not";
}

TEST_F(WaylandVsyncGrid, GridDepthIsClamped) {
  vsync_.SetGridDepth(0);
  Present(kVblank);
  vsync_.SetInFlightForTest(1);
  EXPECT_FALSE(Request(kVblank + 1 * kMs)) << "0 means 1";
  vsync_.SetGridDepth(99);
  vsync_.SetInFlightForTest(2);
  EXPECT_FALSE(Request(kVblank + 2 * kPeriod + 1 * kMs)) << "99 means 2";
}

TEST_F(WaylandVsyncGrid, ADiscardInARefreshWithAFrameWaits) {
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 2 * kMs));
  now_ = kVblank + 4 * kMs;
  ivi::WpFeedbackHandler fb(&vsync_);
  vsync_.OnDiscarded(&fb);
  EXPECT_TRUE(vsync_.HasParkedBaton());
}

TEST_F(WaylandVsyncGrid, NoRefreshPeriodLeavesPresentationPacing) {
  ivi::WaylandVsyncProvider vsync;
  vsync.SetEngine(nullptr, runner_.get());
  vsync.SetGridPacing(true);
  std::vector<uint64_t> scheduled;
  uint64_t now = kVblank;
  vsync.SetPacingClockForTest([&] { return now; },
                              [&](uint64_t due) { scheduled.push_back(due); });
  ivi::WpFeedbackHandler fb(&vsync);
  vsync.OnPresented(kVblank, /*refresh_ns=*/0, 0, 0, &fb);
  now = kVblank + 1 * kMs;
  vsync.SubmitBaton(nullptr, kBaton);
  EXPECT_FALSE(vsync.HasParkedBaton());
  now = kVblank + 2 * kMs;
  vsync.SubmitBaton(nullptr, kBaton);
  EXPECT_FALSE(vsync.HasParkedBaton()) << "not paced to a guessed 60 Hz";
  // A second presentation, still without a period, and a commit in flight:
  // presentation pacing holds the baton for it, grid pacing would not.
  vsync.OnPresented(kVblank + 16 * kMs, /*refresh_ns=*/0, 0, 0, &fb);
  vsync.SetInFlightForTest(1);
  now = kVblank + 17 * kMs;
  vsync.SubmitBaton(nullptr, kBaton);
  EXPECT_TRUE(vsync.HasParkedBaton()) << "feedback-paced: waits for it";
  EXPECT_TRUE(scheduled.empty());
  vsync.Stop();
}

TEST_F(WaylandVsyncGrid, ANewRefreshPeriodStartsTheLeadOver) {
  Present(kVblank);
  vsync_.NoteLatch(kVblank + 2 * kMs);
  ASSERT_EQ(vsync_.LatchLeadNs(), kPeriod - 2 * kMs);
  constexpr uint32_t kFast = 8'333'333;
  const uint64_t at = kVblank + 10 * kPeriod;
  now_ = at;
  ivi::WpFeedbackHandler fb(&vsync_);
  vsync_.OnPresented(at, kFast, 0, 0, &fb);
  vsync_.NoteLatch(at + 3 * kMs);
  EXPECT_EQ(vsync_.LatchLeadNs(), kFast - 3 * kMs) << "not the old, clamped";
}

TEST_F(WaylandVsyncGrid, StopForgetsTheLatchLead) {
  Present(kVblank);
  vsync_.NoteLatch(kVblank + 2 * kMs);
  ASSERT_NE(vsync_.LatchLeadNs(), 0U);
  vsync_.Stop();
  EXPECT_EQ(vsync_.LatchLeadNs(), 0U);
}

TEST_F(WaylandVsyncGrid, ANegativePhaseHandsFramesOutBeforeTheVblank) {
  vsync_.SetHandOutPhase(-5 * kMs);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs)) << "idle: answered at once";
  ASSERT_FALSE(Request(kVblank + 3 * kMs));
  ASSERT_EQ(scheduled_.size(), 1U);
  EXPECT_EQ(scheduled_[0], kVblank + kPeriod - 5 * kMs);
  now_ = kVblank + kPeriod - 5 * kMs;
  vsync_.FireDeferredForTest();
  EXPECT_FALSE(vsync_.HasParkedBaton()) << "handed out before the vblank";
  // No second frame for that refresh: the presentation ending it leaves the
  // next baton for the next hand-out.
  ASSERT_FALSE(Request(kVblank + kPeriod - 2 * kMs));
  Present(kVblank + kPeriod);
  EXPECT_TRUE(vsync_.HasParkedBaton());
  EXPECT_EQ(scheduled_.back(), kVblank + 2 * kPeriod - 5 * kMs);
}

TEST_F(WaylandVsyncGrid, APositivePhaseHandsFramesOutAfterTheVblank) {
  vsync_.SetHandOutPhase(2 * kMs);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 3 * kMs));
  EXPECT_EQ(scheduled_.back(), kVblank + kPeriod + 2 * kMs);
  // The presentation on the vblank comes before the hand-out time: the baton
  // waits for it.
  Present(kVblank + kPeriod);
  EXPECT_TRUE(vsync_.HasParkedBaton());
  now_ = kVblank + kPeriod + 2 * kMs;
  vsync_.FireDeferredForTest();
  EXPECT_FALSE(vsync_.HasParkedBaton());
}

TEST_F(WaylandVsyncGrid, TheStartWindowFollowsTheHandOutTime) {
  vsync_.SetHandOutPhase(-5 * kMs);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  // A request 1 ms after the next refresh's hand-out, with no frame started
  // for it, starts at once; the one after waits for the following hand-out.
  EXPECT_TRUE(Request(kVblank + kPeriod - 4 * kMs));
  EXPECT_FALSE(Request(kVblank + kPeriod - 1 * kMs));
  EXPECT_EQ(scheduled_.back(), kVblank + 2 * kPeriod - 5 * kMs);
}

TEST_F(WaylandVsyncGrid, ARequestPastTheStartWindowWaitsForTheNextHandOut) {
  vsync_.SetHandOutPhase(-5 * kMs);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + kPeriod - 2 * kMs))
      << "3 ms past the hand-out";
  EXPECT_EQ(scheduled_.back(), kVblank + 2 * kPeriod - 5 * kMs);
}

TEST_F(WaylandVsyncGrid, ThePhaseIsClampedToEightMilliseconds) {
  vsync_.SetHandOutPhase(-20 * kMs);
  Present(kVblank);
  ASSERT_TRUE(Request(kVblank + 1 * kMs));
  ASSERT_FALSE(Request(kVblank + 3 * kMs));
  EXPECT_EQ(scheduled_.back(),
            kVblank + kPeriod - ivi::WaylandVsyncProvider::kMaxHandOutPhaseNs);
}

TEST_F(WaylandVsyncGrid, OffTheGridThePhaseDoesNotApply) {
  vsync_.SetGridPacing(false);
  vsync_.SetHandOutPhase(-5 * kMs);
  EXPECT_EQ(vsync_.PaceHandOutPhaseNs(), 0);
  vsync_.SetGridPacing(true);
  Present(kVblank);  // the grid starts with the first refresh period
  EXPECT_EQ(vsync_.PaceHandOutPhaseNs(), -5 * kMs);
}
