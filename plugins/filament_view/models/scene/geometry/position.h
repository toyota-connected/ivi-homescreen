/*
 * Copyright 2020-2023 Toyota Connected North America
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

#include <flutter/encodable_value.h>

#include <optional>

namespace plugin_filament_view {

class Position {
 public:
  Position(float x, float y, float z);

  explicit Position(const flutter::EncodableMap& params);

  [[nodiscard]] float getX() const { return x_; }
  [[nodiscard]] float getY() const { return y_; }
  [[nodiscard]] float getZ() const { return z_; }

  void Print(const char* tag) const;

  // Disallow copy and assign.
  Position(const Position&) = delete;
  Position& operator=(const Position&) = delete;

 private:
  float x_;
  float y_;
  float z_;
};

}  // namespace plugin_filament_view
