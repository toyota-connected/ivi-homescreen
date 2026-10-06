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

// ihs::TouchFrame, the wl_touch per-scan accumulator.
//
// This exists because #714 did not. That change added a two-boolean state
// machine to display.cc to cover weston <= 11 dropping the frame after an up,
// and it was only ever exercised by hand -- Display needs a live compositor to
// construct, so nothing in ctest reached it. It worked for one finger and broke
// every multi-contact scan, which is what a customer reported on a ten-point
// controller. Hence the split, and hence these cases.
//
// The invariant under test throughout: one hardware scan reaches the engine as
// one batch, under one timestamp, however many contacts it carries and
// whichever compositor sent it.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "shell/wayland/touch_frame.h"

namespace {

struct Batch {
  std::vector<ihs::TouchEvent> events;
  uint64_t frame_time_us;
};

class Recorder {
 public:
  ihs::TouchFrame::Sink Sink() {
    return [this](const ihs::TouchEvent* events, const std::size_t count,
                  const uint64_t frame_time_us) {
      batches_.push_back({{events, events + count}, frame_time_us});
    };
  }
  [[nodiscard]] const std::vector<Batch>& batches() const { return batches_; }

  /// Phases of one batch, as a string, for readable expectations:
  /// "d0 d1" is down on device 0 then device 1.
  [[nodiscard]] std::string shape(const size_t i) const {
    std::string out;
    for (const auto& e : batches_[i].events) {
      if (!out.empty()) {
        out += ' ';
      }
      switch (e.phase) {
        case kDown:
          out += 'd';
          break;
        case kUp:
          out += 'u';
          break;
        case kMove:
          out += 'm';
          break;
        case kCancel:
          out += 'c';
          break;
        default:
          out += '?';
          break;
      }
      out += std::to_string(e.device);
    }
    return out;
  }

 private:
  std::vector<Batch> batches_;
};

constexpr std::size_t kNoCap = 0;

// The case the customer hit. One scan: contact 0 lifts while 1 and 2 keep
// moving. #714 flushed inside up, so this arrived as [u0] then [m1 m2] -- and
// because the motions cleared its "an up is outstanding" flag, the frame never
// latched "this compositor sends frames after up" and the split became
// permanent for the session.
TEST(TouchFrame, AScanWhereOneContactLiftsIsStillOneBatch) {
  Recorder rec;
  ihs::TouchFrame tf(rec.Sink(), kNoCap);
  for (int32_t id = 0; id < 3; ++id) {
    tf.Down(id, 10.0 * id, 20.0, 100);
  }
  tf.Frame();
  tf.Up(0, 200);
  tf.Motion(1, 11.0, 21.0, 200);
  tf.Motion(2, 22.0, 22.0, 200);
  tf.Frame();

  ASSERT_EQ(rec.batches().size(), 2U)
      << "the scan was split; one wl_touch.frame is one batch";
  EXPECT_EQ(rec.shape(0), "d0 d1 d2");
  EXPECT_EQ(rec.shape(1), "u0 m1 m2");
}

// Ten fingers, two lifting in the same scan: the configuration the ft5x06 on a
// DSI panel produces, and the one #714 turned into three submissions.
TEST(TouchFrame, TenContactsWithTwoLiftingAreOneBatch) {
  Recorder rec;
  ihs::TouchFrame tf(rec.Sink(), kNoCap);
  for (int32_t id = 0; id < 10; ++id) {
    tf.Down(id, id, id, 100);
  }
  tf.Frame();
  tf.Up(0, 300);
  tf.Up(1, 300);
  for (int32_t id = 2; id < 10; ++id) {
    tf.Motion(id, id + 0.5, id + 0.5, 300);
  }
  tf.Frame();

  ASSERT_EQ(rec.batches().size(), 2U);
  EXPECT_EQ(rec.batches()[0].events.size(), 10U);
  EXPECT_EQ(rec.shape(1), "u0 u1 m2 m3 m4 m5 m6 m7 m8 m9")
      << "ten contacts of one scan must reach the engine together";
  EXPECT_EQ(rec.batches()[1].frame_time_us, 300U)
      << "one scan is one hardware moment, so one stamp";
}

// weston <= 11 clears the touch focus in its up handler, so the frame after an
// up is never sent (weston!980). The release must not sit in the accumulator
// until the next contact: the dispatch round closes it out.
TEST(TouchFrame, AScanWhoseFrameNeverComesIsFlushedWhenTheRoundDrains) {
  Recorder rec;
  ihs::TouchFrame tf(rec.Sink(), kNoCap);
  tf.Down(0, 1.0, 2.0, 100);
  tf.Frame();
  ASSERT_EQ(rec.batches().size(), 1U);

  tf.Up(0, 200);  // no Frame() -- the compositor dropped it
  EXPECT_EQ(rec.batches().size(), 1U) << "nothing should go out mid-scan";
  tf.EndOfDispatch();

  ASSERT_EQ(rec.batches().size(), 2U) << "the up is stranded";
  EXPECT_EQ(rec.shape(1), "u0");
}

// Same compositor, but the whole multi-contact scan has no frame: it still
// arrives as one batch rather than one per contact.
TEST(TouchFrame, AFramelessMultiContactScanIsOneBatch) {
  Recorder rec;
  ihs::TouchFrame tf(rec.Sink(), kNoCap);
  for (int32_t id = 0; id < 3; ++id) {
    tf.Down(id, id, id, 100);
  }
  tf.EndOfDispatch();
  ASSERT_EQ(rec.batches().size(), 1U);
  EXPECT_EQ(rec.shape(0), "d0 d1 d2");

  tf.Up(0, 200);
  tf.Motion(1, 5.0, 5.0, 200);
  tf.EndOfDispatch();
  ASSERT_EQ(rec.batches().size(), 2U);
  EXPECT_EQ(rec.shape(1), "u0 m1");
}

// On a compositor that does send the frame, the round-end flush must cost
// nothing -- no empty batch, no second delivery.
TEST(TouchFrame, EndOfDispatchAfterAFrameDeliversNothingExtra) {
  Recorder rec;
  ihs::TouchFrame tf(rec.Sink(), kNoCap);
  tf.Down(0, 1.0, 2.0, 100);
  tf.Frame();
  tf.EndOfDispatch();
  tf.EndOfDispatch();
  EXPECT_EQ(rec.batches().size(), 1U);
}

// The stamp is the scan's, taken from the first contact event that carried one,
// and it resets with the batch. A later contact's stamp must not retroactively
// become the frame's.
TEST(TouchFrame, TheFrameStampIsTheFirstOneSeenAndResetsPerBatch) {
  Recorder rec;
  ihs::TouchFrame tf(rec.Sink(), kNoCap);
  tf.Down(0, 0.0, 0.0, 0);    // no high-res stamp available
  tf.Down(1, 1.0, 1.0, 700);  // this one has it
  tf.Frame();
  tf.Down(2, 2.0, 2.0, 900);
  tf.Frame();

  ASSERT_EQ(rec.batches().size(), 2U);
  EXPECT_EQ(rec.batches()[0].frame_time_us, 700U);
  EXPECT_EQ(rec.batches()[1].frame_time_us, 900U);
}

// An up or motion for an id with no prior down is dropped: the engine was never
// told that device went down, and inventing one trips the packet converter.
TEST(TouchFrame, AnUnknownContactIdIsDropped) {
  Recorder rec;
  ihs::TouchFrame tf(rec.Sink(), kNoCap);
  tf.Up(7, 100);
  tf.Motion(7, 1.0, 1.0, 100);
  tf.EndOfDispatch();
  EXPECT_TRUE(rec.batches().empty());
  EXPECT_EQ(tf.active(), 0U);
}

// wl_touch.cancel ends the session for every active contact, not just the
// first. A contact left down trips FML_DCHECK(!state.is_down) in the engine's
// packet converter on that device's next down.
TEST(TouchFrame, CancelTerminatesEveryActiveContact) {
  Recorder rec;
  ihs::TouchFrame tf(rec.Sink(), kNoCap);
  for (int32_t id = 0; id < 4; ++id) {
    tf.Down(id, id, id, 100);
  }
  tf.Frame();
  tf.Motion(0, 9.0, 9.0, 200);  // pending, and moot once canceled
  tf.Cancel();

  ASSERT_EQ(rec.batches().size(), 2U) << "cancel delivers immediately";
  EXPECT_EQ(rec.batches()[1].events.size(), 4U)
      << "every contact that was down must be canceled";
  for (const auto& e : rec.batches()[1].events) {
    EXPECT_EQ(e.phase, kCancel);
  }
  EXPECT_EQ(rec.batches()[1].frame_time_us, 0U)
      << "synthesized cancels have no hardware moment";
  EXPECT_EQ(tf.active(), 0U);
}

// Nothing in the protocol forces a compositor to send frame at all, so the
// accumulator must not grow without bound.
TEST(TouchFrame, TheBatchCapBoundsAFramelessStream) {
  Recorder rec;
  ihs::TouchFrame tf(rec.Sink(), 4);
  for (int32_t id = 0; id < 4; ++id) {
    tf.Down(id, id, id, 100);
  }
  EXPECT_EQ(rec.batches().size(), 1U) << "the cap should have flushed";
  EXPECT_EQ(rec.batches()[0].events.size(), 4U);
  EXPECT_EQ(tf.pending(), 0U);
}

}  // namespace
