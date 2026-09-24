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
