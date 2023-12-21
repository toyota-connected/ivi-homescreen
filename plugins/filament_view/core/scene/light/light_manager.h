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

#include <future>

#include "core/scene/geometry/direction.h"
#include "core/scene/geometry/position.h"
#include "core/scene/light/light.h"
#include "viewer/custom_model_viewer.h"
#include "core/include/resource.h"

namespace plugin_filament_view {

class CustomModelViewer;

class Light;

class LightManager {
 public:
  explicit LightManager(CustomModelViewer* modelViewer);

  void setDefaultLight();

  std::future<Resource<std::string>> changeLight(Light* light);

  // Disallow copy and assign.
  LightManager(const LightManager&) = delete;

  LightManager& operator=(const LightManager&) = delete;

 private:
  CustomModelViewer* modelViewer_;
  ::filament::Engine* engine_;
  utils::Entity entityLight_;
};
}  // namespace plugin_filament_view