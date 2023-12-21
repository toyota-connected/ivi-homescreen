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

#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

#include "core/scene/geometry/direction.h"
#include "core/scene/geometry/position.h"
#include "core/scene/geometry/size.h"
#include "core/scene/ground_manager.h"
#include "core/scene/material/material.h"

namespace plugin_filament_view {

class GroundManager;

class Ground {
 public:
  Ground(const std::string& flutter_assets_path,
         const flutter::EncodableMap& params);

  void Print(const char* tag);

  // Disallow copy and assign.
  Ground(const Ground&) = delete;

  Ground& operator=(const Ground&) = delete;

  friend class GroundManager;

 private:
  const std::string& flutterAssetsPath_;

  std::unique_ptr<Position> center_position_;
  std::unique_ptr<Direction> normal_;
  std::unique_ptr<Size> size_;
  std::unique_ptr<Material> material_;
  bool isBelowModel_;
};
}  // namespace plugin_filament_view