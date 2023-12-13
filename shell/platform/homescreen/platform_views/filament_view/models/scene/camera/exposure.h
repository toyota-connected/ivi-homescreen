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

#include "encodable_value.h"

#include "shell/engine.h"

namespace view_filament_view {
class Exposure {
 public:
  Exposure(void* parent,
           const std::string& flutter_assets_path,
           const flutter::EncodableMap& params);
  void Print(const char* tag);
  FML_DISALLOW_COPY_AND_ASSIGN(Exposure);

 private:
  void* parent_;
  const std::string& flutterAssetsPath_;

  /// Aperture in f-stops, clamped between 0.5 and 64. A lower aperture value
  /// increases the exposure, leading to a brighter scene. Realistic values are
  /// between 0.95 and 32.
  std::optional<double> aperture_;

  /// shutterSpeed – Shutter speed in seconds,
  /// clamped between 1/25,000 and 60. A lower shutter speed increases the
  /// exposure. Realistic values are between 1/8000 and 30. sensitivity
  std::optional<double> shutterSpeed_;

  /// Sensitivity in ISO, clamped between 10 and 204,800.
  /// A higher sensitivity increases the exposure. Realistic values are between
  /// 50 and 25600.
  std::optional<double> sensitivity_;

  /// Sets this camera's exposure directly.
  std::optional<double> exposure_;
};
}  // namespace view_filament_view