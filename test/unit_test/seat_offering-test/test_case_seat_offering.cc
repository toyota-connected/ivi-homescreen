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
#include <vector>

#include <gtest/gtest.h>

#include "wayland/seat_offering.h"

namespace {

// WL_SEAT_CAPABILITY_*, without the Wayland headers.
constexpr uint32_t kPointer = 1;
constexpr uint32_t kKeyboard = 2;
constexpr uint32_t kTouch = 4;

struct FakeSeat {};

struct Seat {
  FakeSeat* seat;
  uint32_t caps;
};

}  // namespace

TEST(SeatOffering, TheFirstSeatThatOffersItWins) {
  FakeSeat local;
  FakeSeat remote_a;
  FakeSeat remote_b;
  const std::vector<Seat> seats = {{&local, kTouch},
                                   {&remote_a, kPointer | kKeyboard},
                                   {&remote_b, kPointer | kKeyboard}};
  EXPECT_EQ(SeatOffering(seats, kTouch), &local);
  EXPECT_EQ(SeatOffering(seats, kPointer), &remote_a);
  EXPECT_EQ(SeatOffering(seats, kKeyboard), &remote_a);
}

TEST(SeatOffering, AnotherSeatTakesOverWhenTheFirstDropsIt) {
  FakeSeat remote_a;
  FakeSeat remote_b;
  std::vector<Seat> seats = {{&remote_a, kPointer | kKeyboard},
                             {&remote_b, kPointer | kKeyboard}};
  // The first remote client disconnects: its seat is removed.
  seats.erase(seats.begin());
  EXPECT_EQ(SeatOffering(seats, kPointer), &remote_b);
  EXPECT_EQ(SeatOffering(seats, kKeyboard), &remote_b);
}

TEST(SeatOffering, ASeatThatDropsACapabilityIsPassedOver) {
  FakeSeat a;
  FakeSeat b;
  std::vector<Seat> seats = {{&a, kPointer}, {&b, kPointer}};
  seats[0].caps = 0;  // its mouse was unplugged
  EXPECT_EQ(SeatOffering(seats, kPointer), &b);
}

TEST(SeatOffering, NoSeatOffersIt) {
  FakeSeat a;
  const std::vector<Seat> seats = {{&a, kKeyboard}};
  EXPECT_EQ(SeatOffering(seats, kTouch), nullptr);
  EXPECT_EQ(SeatOffering(std::vector<Seat>{}, kPointer), nullptr);
}
