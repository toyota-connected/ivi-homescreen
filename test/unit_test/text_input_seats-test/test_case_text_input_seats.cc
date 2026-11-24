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

#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "wayland/text_input_seats.h"

namespace {

using wl::ime::ContentHint;
using wl::ime::ContentPurpose;

// Records what a text input was asked to send, in order.
class FakeTextInput {
 public:
  explicit FakeTextInput(std::vector<std::string>* log) : log_(log) {}

  void Activate() { log_->emplace_back("enable"); }
  void Deactivate() { log_->emplace_back("disable"); }
  void SetContentType(ContentHint /*hint*/, ContentPurpose purpose) {
    log_->push_back("content " +
                    std::to_string(static_cast<uint32_t>(purpose)));
  }
  void SetCursorRectangle(int32_t x, int32_t y, int32_t w, int32_t h) {
    log_->push_back("rect " + std::to_string(x) + "," + std::to_string(y) +
                    " " + std::to_string(w) + "x" + std::to_string(h));
  }
  void SetSurroundingText(std::string_view utf8, uint32_t cursor, uint32_t) {
    log_->push_back("surrounding " + std::string(utf8) + "@" +
                    std::to_string(cursor));
  }
  void Commit() { log_->emplace_back("commit"); }

 private:
  std::vector<std::string>* log_;
};

using Seats = TextInputSeats<FakeTextInput>;

constexpr auto kNumber = static_cast<ContentPurpose>(2);

int seat_a = 0;
int seat_b = 0;

}  // namespace

TEST(TextInputSeats, EveryRequestGoesToEverySeat) {
  std::vector<std::string> a;
  std::vector<std::string> b;
  Seats seats;
  seats.Add(&seat_a, std::make_unique<FakeTextInput>(&a));
  seats.Add(&seat_b, std::make_unique<FakeTextInput>(&b));
  // Nothing is sent before a field is focused.
  EXPECT_TRUE(a.empty());

  seats.Activate(ContentHint{}, kNumber, 1, 2, 3, 4);
  seats.SetCursorRectangle(5, 6, 7, 8);
  seats.Deactivate();

  const std::vector<std::string> want = {
      "enable",       "content 2", "rect 1,2 3x4", "commit",
      "rect 5,6 7x8", "commit",    "disable",      "commit"};
  EXPECT_EQ(a, want);
  EXPECT_EQ(b, want);
}

TEST(TextInputSeats, EnableGoesBeforeTheStateItResets) {
  std::vector<std::string> a;
  Seats seats;
  seats.Add(&seat_a, std::make_unique<FakeTextInput>(&a));
  seats.Activate(ContentHint{}, kNumber, 1, 2, 3, 4);
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a.front(), "enable");
  EXPECT_EQ(a.back(), "commit");
}

TEST(TextInputSeats, ASeatThatArrivesWhileAFieldIsFocusedGetsIt) {
  std::vector<std::string> b;
  Seats seats;
  seats.Activate(ContentHint{}, kNumber, 1, 2, 3, 4);
  seats.SetSurroundingText("hi", 2, 2);
  seats.Add(&seat_b, std::make_unique<FakeTextInput>(&b));
  const std::vector<std::string> want = {"enable", "content 2", "rect 1,2 3x4",
                                         "surrounding hi@2", "commit"};
  EXPECT_EQ(b, want);
}

TEST(TextInputSeats, AnEnterResendsTheStateEnableReset) {
  std::vector<std::string> a;
  Seats seats;
  seats.Add(&seat_a, std::make_unique<FakeTextInput>(&a));
  // Nothing focused: an enter has nothing to send.
  seats.OnEnter();
  EXPECT_TRUE(a.empty());

  seats.Activate(ContentHint{}, kNumber, 1, 2, 3, 4);
  a.clear();
  seats.OnEnter();
  const std::vector<std::string> want = {"content 2", "rect 1,2 3x4", "commit"};
  EXPECT_EQ(a, want);

  // After the field is blurred an enter sends nothing again.
  seats.Deactivate();
  a.clear();
  seats.OnEnter();
  EXPECT_TRUE(a.empty());
}

TEST(TextInputSeats, ARemovedSeatIsNoLongerSent) {
  std::vector<std::string> a;
  std::vector<std::string> b;
  Seats seats;
  seats.Add(&seat_a, std::make_unique<FakeTextInput>(&a));
  seats.Add(&seat_b, std::make_unique<FakeTextInput>(&b));
  seats.Remove(&seat_a);
  EXPECT_FALSE(seats.Has(&seat_a));
  EXPECT_TRUE(seats.Has(&seat_b));
  EXPECT_EQ(seats.size(), 1U);

  seats.Activate(ContentHint{}, kNumber, 1, 2, 3, 4);
  EXPECT_TRUE(a.empty());
  EXPECT_FALSE(b.empty());
}

TEST(TextInputSeats, AddingASeatAgainReplacesItsTextInput) {
  std::vector<std::string> first;
  std::vector<std::string> second;
  Seats seats;
  seats.Add(&seat_a, std::make_unique<FakeTextInput>(&first));
  seats.Add(&seat_a, std::make_unique<FakeTextInput>(&second));
  EXPECT_EQ(seats.size(), 1U);
  seats.Activate(ContentHint{}, kNumber, 1, 2, 3, 4);
  EXPECT_TRUE(first.empty());
  EXPECT_FALSE(second.empty());
}

TEST(TextInputSeats, ANullTextInputIsNotAdded) {
  Seats seats;
  seats.Add(&seat_a, nullptr);
  EXPECT_EQ(seats.size(), 0U);
}
