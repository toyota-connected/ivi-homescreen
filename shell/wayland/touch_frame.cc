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

#include "shell/wayland/touch_frame.h"

namespace ihs {

void TouchFrame::Push(const TouchEvent& ev, const uint64_t ts_us) {
  // Latch the frame's shared stamp from the first contact event that carried
  // one; the rest of the scan is the same hardware moment.
  if (frame_time_us_ == 0) {
    frame_time_us_ = ts_us;
  }
  frame_.push_back(ev);
  if (max_batch_ != 0 && frame_.size() >= max_batch_) {
    Flush();
  }
}

void TouchFrame::Down(const int32_t id,
                      const double x,
                      const double y,
                      const uint64_t ts_us) {
  points_[id] = {x, y};
  Push({kDown, x, y, id}, ts_us);
}

void TouchFrame::Motion(const int32_t id,
                        const double x,
                        const double y,
                        const uint64_t ts_us) {
  const auto it = points_.find(id);
  if (it == points_.end()) {
    return;
  }
  it->second = {x, y};
  Push({kMove, x, y, id}, ts_us);
}

void TouchFrame::Up(const int32_t id, const uint64_t ts_us) {
  const auto it = points_.find(id);
  if (it == points_.end()) {
    return;
  }
  Push({kUp, it->second.first, it->second.second, id}, ts_us);
  points_.erase(it);
}

void TouchFrame::Cancel() {
  // Pending updates for this session are moot, and the synthesized cancels have
  // no hardware moment -- drop the latched stamp so the flush uses arrival
  // time.
  frame_.clear();
  frame_time_us_ = 0;
  for (const auto& [id, pos] : points_) {
    frame_.push_back({kCancel, pos.first, pos.second, id});
  }
  points_.clear();
  Flush();
}

void TouchFrame::Frame() {
  Flush();
}

void TouchFrame::Flush() {
  if (frame_.empty()) {
    return;
  }
  if (sink_) {
    sink_(frame_.data(), frame_.size(), frame_time_us_);
  }
  frame_.clear();
  frame_time_us_ = 0;
}

}  // namespace ihs
