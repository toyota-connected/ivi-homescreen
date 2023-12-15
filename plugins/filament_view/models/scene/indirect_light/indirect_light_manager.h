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

#include "indirect_light.h"
#include "models/scene/geometry/direction.h"
#include "models/scene/geometry/position.h"
#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {
class IndirectLightManager {
 public:
  IndirectLightManager(void* context,
                       CustomModelViewer* model_viewer,
                       const std::string& flutter_assets_path);
  void setDefaultIndirectLight(IndirectLight* indirect_light);

  // Disallow copy and assign.
  IndirectLightManager(const IndirectLightManager&) = delete;
  IndirectLightManager& operator=(const IndirectLightManager&) = delete;

 private:
  void* context_;
  CustomModelViewer* model_viewer_;
  const std::string& flutterAssetsPath_;
};
}  // namespace plugin_filament_view