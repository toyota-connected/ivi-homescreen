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

#include "ground.h"
#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {
class GroundManager {
 public:
  GroundManager(CustomModelViewer* model_viewer,
                const std::string& flutter_assets_path);
  void Print(const char* tag);

  void createGround(Ground* ground);

  // Disallow copy and assign.
  GroundManager(const GroundManager&) = delete;
  GroundManager& operator=(const GroundManager&) = delete;

 private:
  CustomModelViewer* model_viewer_;
  const std::string& flutterAssetsPath_;
};
}  // namespace plugin_filament_view