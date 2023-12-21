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

#include <filament/Engine.h>
#include <filament/IndirectLight.h>
#include <filament/LightManager.h>

#include "core/scene/geometry/direction.h"
#include "core/scene/geometry/position.h"
#include "core/scene/indirect_light/indirect_light.h"
#include "core/utils/ibl_profiler.h"
#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"
#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {

class CustomModelViewer;

class IBLProfiler;

class IndirectLight;

class DefaultIndirectLight;

class KtxIndirectLight;

class HdrIndirectLight;

class IndirectLightManager {
 public:
  IndirectLightManager(CustomModelViewer* model_viewer,
                       IBLProfiler* ibl_profiler);

  void setDefaultIndirectLight();

  std::future<std::string> setIndirectLightFromKtxAsset(std::string path,
                                                        double intensity);

  std::future<std::string> setIndirectLightFromKtxUrl(std::string url,
                                                      double intensity);

  std::future<std::string> setIndirectLightFromHdrAsset(std::string path,
                                                        double intensity);

  std::future<std::string> setIndirectLightFromHdrUrl(std::string url,
                                                      double intensity);

  std::string loadIndirectLightHdrFromFile(const std::string& asset_path,
                                           double intensity);

  std::future<std::string> setIndirectLight(
      DefaultIndirectLight* indirectLight);

  // Disallow copy and assign.
  IndirectLightManager(const IndirectLightManager&) = delete;

  IndirectLightManager& operator=(const IndirectLightManager&) = delete;

 private:
  CustomModelViewer* model_viewer_;
  IBLProfiler* ibl_prefilter_;
  ::filament::Engine* engine_;
};
}  // namespace plugin_filament_view