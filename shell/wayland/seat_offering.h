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

#pragma once

#include <cstdint>

// Which seat a pointer, keyboard or touch is taken from. The Wayland backend
// holds one of each, so when the seat holding one drops it -- the device is
// unplugged, or weston removes a remote client's seat when it disconnects --
// the same kind is taken from another seat that still offers it. Seats are
// kept in the order they were advertised, and the first that offers it wins,
// as at startup.
//
// @p seats is a range of records with a `seat` handle and a `caps` mask of
// WL_SEAT_CAPABILITY_* bits. Returns the first seat whose caps include @p cap,
// or a null handle when none does.
template <typename Seats>
auto SeatOffering(const Seats& seats, const uint32_t cap)
    -> decltype(seats.begin()->seat) {
  for (const auto& s : seats) {
    if ((s.caps & cap) != 0U) {
      return s.seat;
    }
  }
  return nullptr;
}
