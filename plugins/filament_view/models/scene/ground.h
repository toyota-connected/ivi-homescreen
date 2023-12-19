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

#include "models/scene/geometry/direction.h"
#include "models/scene/geometry/position.h"
#include "models/scene/geometry/size.h"
#include "models/scene/material/material.h"

namespace plugin_filament_view {
class Ground {
 public:
  Ground(void* parent,
         const std::string& flutter_assets_path,
         const flutter::EncodableMap& params);

  [[nodiscard]] std::optional<Position*> getCenterPosition() const {
    return center_position_wrapper_;
  }
  [[nodiscard]] std::optional<Direction*> getNormal() const {
    return normal_wrapper_;
  }
  [[nodiscard]] std::optional<Size*> getSize() const { return size_wrapper_; }
  [[nodiscard]] std::optional<Material*> getMaterial() const {
    return material_wrapper_;
  }
  [[nodiscard]] std::optional<bool> getIsBelowModel() const {
    return isBelowModel_;
  }

  void Print(const char* tag);

  // Disallow copy and assign.
  Ground(const Ground&) = delete;
  Ground& operator=(const Ground&) = delete;

 private:
  void* parent_;
  const std::string& flutterAssetsPath_;

  std::optional<std::unique_ptr<Position>> center_position_;
  std::optional<Position*> center_position_wrapper_;
  std::optional<std::unique_ptr<Direction>> normal_;
  std::optional<Direction*> normal_wrapper_;
  std::optional<std::unique_ptr<Size>> size_;
  std::optional<Size*> size_wrapper_;
  std::optional<std::unique_ptr<Material>> material_;
  std::optional<Material*> material_wrapper_;
  bool isBelowModel_;
};
}  // namespace plugin_filament_view