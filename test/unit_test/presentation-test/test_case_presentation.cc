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

#include <cstdint>
#include <memory>
#include <vector>

#include "gtest/gtest.h"

#include "view/presentation.h"

namespace {

// Records what it is told.
class RecordingSink : public IPresentationSink {
 public:
  struct Report {
    uint64_t frame;
    PresentationTime when;
  };
  void OnPresented(uint64_t frame, const PresentationTime& when) override {
    reports.push_back({frame, when});
  }
  std::vector<Report> reports;
};

PresentationTime At(uint64_t msc) {
  PresentationTime t;
  t.ust_ns = msc * 16'666'667ULL;
  t.refresh_ns = 16'666'667U;
  t.msc = msc;
  t.flags = kPresentedVsync | kPresentedHwClock | kPresentedHwCompletion;
  return t;
}

}  // namespace

TEST(Presentation, ACommittedFrameIsReportedWhenShown) {
  PresentationTracker tracker;
  auto sink = std::make_shared<RecordingSink>();
  tracker.Note(sink, 5, /*zero_copy=*/false);
  tracker.Commit(1);
  EXPECT_TRUE(sink->reports.empty()) << "reported before it was shown";

  tracker.Presented(1, At(100));
  ASSERT_EQ(sink->reports.size(), 1U);
  EXPECT_EQ(sink->reports[0].frame, 5U);
  EXPECT_EQ(sink->reports[0].when.msc, 100U);
  EXPECT_EQ(sink->reports[0].when.flags & kPresentedZeroCopy, 0U);
  EXPECT_EQ(tracker.in_flight(), 0U);

  // Shown once; a second event for the same commit finds nothing.
  tracker.Presented(1, At(101));
  EXPECT_EQ(sink->reports.size(), 1U);
}

// A view drawn from several layers is as old as its oldest layer, and
// zero-copy only when every layer was scanned out as is.
TEST(Presentation, LayersOfOneViewMerge) {
  PresentationTracker tracker;
  auto sink = std::make_shared<RecordingSink>();
  tracker.Note(sink, 9, /*zero_copy=*/true);
  tracker.Note(sink, 8, /*zero_copy=*/true);
  tracker.Commit(1);
  tracker.Presented(1, At(1));
  ASSERT_EQ(sink->reports.size(), 1U);
  EXPECT_EQ(sink->reports[0].frame, 8U);
  EXPECT_NE(sink->reports[0].when.flags & kPresentedZeroCopy, 0U);

  tracker.Note(sink, 10, /*zero_copy=*/true);
  tracker.Note(sink, 10, /*zero_copy=*/false);
  tracker.Commit(2);
  tracker.Presented(2, At(2));
  ASSERT_EQ(sink->reports.size(), 2U);
  EXPECT_EQ(sink->reports[1].when.flags & kPresentedZeroCopy, 0U)
      << "one composited layer makes the view composited";
}

// A commit overtaken before it was shown -- the display went straight to a
// later one -- is dropped, not reported late.
TEST(Presentation, AnOvertakenCommitIsDropped) {
  PresentationTracker tracker;
  auto a = std::make_shared<RecordingSink>();
  auto b = std::make_shared<RecordingSink>();
  tracker.Note(a, 1, false);
  tracker.Commit(10);
  tracker.Note(b, 2, false);
  tracker.Commit(11);
  tracker.Presented(11, At(7));
  EXPECT_TRUE(a->reports.empty());
  ASSERT_EQ(b->reports.size(), 1U);
  EXPECT_EQ(tracker.in_flight(), 0U);
  // The overtaken commit's event, arriving late, finds nothing.
  tracker.Presented(10, At(8));
  EXPECT_TRUE(a->reports.empty());
}

TEST(Presentation, DiscardedAndEmptyFramesAreNotQueued) {
  PresentationTracker tracker;
  auto sink = std::make_shared<RecordingSink>();
  tracker.Note(sink, 3, false);
  tracker.Discard();
  tracker.Commit(1);
  EXPECT_EQ(tracker.in_flight(), 0U) << "a discarded frame was committed";
  tracker.Presented(1, At(1));
  EXPECT_TRUE(sink->reports.empty());

  // Untracked frames (token 0) and absent sinks note nothing.
  tracker.Note(sink, 0, false);
  tracker.Note(nullptr, 4, false);
  tracker.Commit(2);
  EXPECT_EQ(tracker.in_flight(), 0U);
}

// A display that stops reporting does not grow the queue without bound.
TEST(Presentation, InFlightIsBounded) {
  PresentationTracker tracker;
  auto sink = std::make_shared<RecordingSink>();
  for (uint64_t i = 1; i <= 50; ++i) {
    tracker.Note(sink, i, false);
    tracker.Commit(i);
  }
  EXPECT_LE(tracker.in_flight(), 4U);
  tracker.Presented(50, At(50));
  ASSERT_EQ(sink->reports.size(), 1U);
  EXPECT_EQ(sink->reports[0].frame, 50U);
}

TEST(Presentation, RefreshPeriodFromAMode) {
  // 1920x1080@60: 148.5 MHz, 2200 x 1125.
  EXPECT_EQ(RefreshPeriodNs(148500, 2200, 1125), 16'666'666U);
  EXPECT_EQ(RefreshPeriodNs(0, 2200, 1125), 0U);
}

// Retire actions -- a producer's buffer handed back -- run once the frame that
// used them is off the display's hands, and exactly once.
TEST(Presentation, RetiresRunWhenTheirCommitIsShown) {
  PresentationTracker tracker;
  int ran = 0;
  tracker.NoteRetire([&ran] { ++ran; });
  tracker.Commit(1);
  EXPECT_EQ(ran, 0) << "ran before the frame was shown";
  tracker.Presented(1, At(1));
  EXPECT_EQ(ran, 1);
  tracker.Presented(1, At(2));
  EXPECT_EQ(ran, 1) << "ran twice";
}

// Overtaken or discarded, a commit is as done with as a shown one: what it
// used is free, even though it is never reported. Without this a hidden
// window's producers never get their buffers back.
TEST(Presentation, RetiresRunForOvertakenAndDiscardedCommits) {
  PresentationTracker tracker;
  int a = 0;
  int b = 0;
  int c = 0;
  tracker.NoteRetire([&a] { ++a; });
  tracker.Commit(1);
  tracker.NoteRetire([&b] { ++b; });
  tracker.Commit(2);
  tracker.Presented(2, At(2));
  EXPECT_EQ(a, 1) << "an overtaken commit's retire did not run";
  EXPECT_EQ(b, 1);

  auto sink = std::make_shared<RecordingSink>();
  tracker.Note(sink, 7, false);
  tracker.NoteRetire([&c] { ++c; });
  tracker.Commit(3);
  tracker.Discarded(3);
  EXPECT_EQ(c, 1) << "a discarded commit's retire did not run";
  EXPECT_TRUE(sink->reports.empty()) << "a discarded frame was reported";
}

// A frame that never reached the display may still have work in flight that
// uses what it noted, so its retires wait for the next frame that does.
TEST(Presentation, ADiscardedFrameCarriesItsRetiresForward) {
  PresentationTracker tracker;
  int ran = 0;
  tracker.NoteRetire([&ran] { ++ran; });
  tracker.Discard();
  tracker.Commit(1);
  EXPECT_EQ(ran, 0);
  tracker.Presented(1, At(1));
  EXPECT_EQ(ran, 1);
}

// Reported early -- the time is the handover, not the display's word -- a
// frame's retires wait exactly one more frame, however long that goes on.
TEST(Presentation, EarlyReportsHoldRetiresOneFrame) {
  PresentationTracker tracker;
  auto sink = std::make_shared<RecordingSink>();
  std::vector<int> ran(4, 0);
  for (size_t i = 0; i < ran.size(); ++i) {
    tracker.Note(sink, i + 1, false);
    tracker.NoteRetire([&ran, i] { ++ran[i]; });
    tracker.Commit(100 + i);
    tracker.PresentedEarly(100 + i, At(i));
    EXPECT_EQ(sink->reports.size(), i + 1)
        << "an early report was not made at once";
    EXPECT_EQ(ran[i], 0) << "frame " << i << " released at once";
    if (i > 0) {
      EXPECT_EQ(ran[i - 1], 1)
          << "frame " << i - 1 << " still held a frame later";
    }
  }
}

// A display that stops answering does not hold buffers forever either.
TEST(Presentation, RetiresRunWhenTheQueueOverflows) {
  PresentationTracker tracker;
  int ran = 0;
  for (uint64_t i = 1; i <= 10; ++i) {
    tracker.NoteRetire([&ran] { ++ran; });
    tracker.Commit(i);
  }
  EXPECT_EQ(ran, 6) << "retires of the commits that fell off did not run";
  EXPECT_LE(tracker.in_flight(), 4U);
}
