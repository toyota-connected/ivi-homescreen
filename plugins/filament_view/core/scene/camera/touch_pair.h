/*
 * Copyright 2024 Toyota Connected North America
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

#include <filament/math/vec2.h>

namespace plugin_filament_view {

class TouchPair {
 public:
  TouchPair()
      : pt0_(filament::math::float2(0.0f)),
        pt1_(filament::math::float2(0.0f)),
        count_(0){};

  TouchPair(int32_t pointer_count,
            const size_t point_data_size,
            const double* point_data,
            uint32_t height) {
    if (pointer_count >= 1) {
      pt0_ = filament::math::float2(
          static_cast<float>(point_data[7]),
          static_cast<float>(height) - static_cast<float>(point_data[8]));
      pt1_ = pt0_;
      count_++;
    }
    if (pointer_count >= 2) {
      pt1_ = filament::math::float2(
          static_cast<float>(point_data[9]),
          static_cast<float>(height) - static_cast<float>(point_data[10]));
      count_++;
    }
  };

  float separation() { return distance(pt0_, pt1_); };
  filament::math::float2 midpoint() { return mix(pt0_, pt1_, 0.5f); };
  int x() { return static_cast<int>(midpoint().x); }
  int y() { return static_cast<int>(midpoint().y); }

 private:
  filament::math::float2 pt0_{};
  filament::math::float2 pt1_{};
  uint32_t count_{};
};

}  // namespace plugin_filament_view
