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

#include <optional>

#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

namespace plugin_filament_view {
class LensProjection {
 public:
  LensProjection(float cameraFocalLength, float aspect);

  explicit LensProjection(const flutter::EncodableMap& params);

  void Print(const char* tag);

  [[nodiscard]] std::optional<float> getFocalLength() const {
    return focalLength_;
  }

  [[nodiscard]] std::optional<float> getAspect() const { return aspect_; }

  [[nodiscard]] std::optional<float> getNear() const { return near_; }

  [[nodiscard]] std::optional<float> getFar() const { return far_; }

  // Disallow copy and assign.
  LensProjection(const LensProjection&) = delete;

  LensProjection& operator=(const LensProjection&) = delete;

 private:
  float focalLength_;
  std::optional<float> aspect_;
  std::optional<float> near_;
  std::optional<float> far_;
};
}  // namespace plugin_filament_view