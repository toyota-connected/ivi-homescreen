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

#include "models/scene/geometry/size.h"
#include "shape.h"
#include "shell/engine.h"

namespace plugin_filament_view {
class Sphere : public Shape {
 public:
  Sphere(void* parent,
         const std::string& flutter_assets_path,
         const flutter::EncodableMap& params);
  void Print(const char* tag);
  FML_DISALLOW_COPY_AND_ASSIGN(Sphere);

 private:
  void* parent_;
  const std::string& flutterAssetsPath_;

  /// The radius of the constructed sphere.
  double radius;
  /// The number of stacks for the sphere.
  std::optional<int> stacks_;
  /// The number of slices for the sphere.
  std::optional<int> slices_;
};
}  // namespace plugin_filament_view