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

#include <future>

#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

#include "core/scene/geometry/direction.h"
#include "core/scene/geometry/position.h"
#include "core/utils/ibl_profiler.h"
#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {
class SkyboxManager {
 public:
  SkyboxManager(CustomModelViewer* modelViewer,
                IBLProfiler* ibl_profiler,
                const std::string& flutter_assets_path);

  std::future<void> Initialize();

  void setDefaultSkybox();

  std::future<Resource<std::string_view>> setSkyboxFromHdrAsset(
      const std::string& path,
      bool showSun,
      bool shouldUpdateLight,
      float intensity);

  std::future<Resource<std::string_view>> setSkyboxFromHdrUrl(
      const std::string& url,
      bool showSun,
      bool shouldUpdateLight,
      float intensity);

  std::future<Resource<std::string_view>> setSkyboxFromKTXAsset(
      const std::string& path);

  std::future<Resource<std::string_view>> setSkyboxFromKTXUrl(
      const std::string& url);

  std::future<Resource<std::string_view>> setSkyboxFromColor(
      const std::string& color);

  Resource<std::string_view> loadSkyboxFromHdrBuffer(
      const std::vector<uint8_t>& buffer,
      bool showSun,
      bool shouldUpdateLight,
      float intensity);

  Resource<std::string_view> loadSkyboxFromHdrFile(const std::string& assetPath,
                                                   bool showSun,
                                                   bool shouldUpdateLight,
                                                   float intensity);

  void destroySkybox();

  // Disallow copy and assign.
  SkyboxManager(const SkyboxManager&) = delete;

  SkyboxManager& operator=(const SkyboxManager&) = delete;

 private:
  CustomModelViewer* modelViewer_;
  ::filament::Engine* engine_;
  IBLProfiler* ibl_profiler_;
  const std::string& flutterAssetsPath_;

  void setTransparentSkybox();
};
}  // namespace plugin_filament_view