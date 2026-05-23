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

#include "display/software_display.h"

#include <utility>

#include "input/iseat.h"

SoftwareDisplay::SoftwareDisplay(const int32_t width,
                                 const int32_t height,
                                 const double refresh_rate_hz)
    : width_(width), height_(height), refresh_rate_hz_(refresh_rate_hz) {}

SoftwareDisplay::~SoftwareDisplay() = default;

void SoftwareDisplay::SetSeat(std::unique_ptr<homescreen::ISeat> seat) {
  seat_ = std::move(seat);
}

void SoftwareDisplay::StartEvents() {
  if (seat_) {
    seat_->Start();
  }
}

void SoftwareDisplay::StopEvents() {
  if (seat_) {
    // Null the seat's view-controller pointer before joining so any
    // in-flight dispatch sees the cleared state and bails out cleanly
    // rather than racing with engine teardown for the lambda chain.
    seat_->SetViewControllerState(nullptr);
    seat_->Stop();
  }
}

void SoftwareDisplay::SetViewControllerState(
    FlutterDesktopViewControllerState* state) {
  view_controller_state_ = state;
  if (seat_) {
    seat_->SetViewControllerState(state);
  }
}
