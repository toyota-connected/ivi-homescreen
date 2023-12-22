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

#include <memory>

#include <filament/MaterialInstance.h>
#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

#include "core/scene/geometry/direction.h"
#include "core/scene/geometry/position.h"
#include "core/scene/material/loader/material_loader.h"
#include "core/scene/material/loader/texture_loader.h"
#include "core/scene/material/model/material.h"
#include "core/scene/material/utils/material_instance.h"
#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {

class CustomModelViewer;

class Material;

class MaterialLoader;

class MaterialInstance;

class TextureLoader;

class MaterialManager {
 public:
  MaterialManager(CustomModelViewer* modelViewer,
                  const std::string& flutter_assets_path);

  Resource<::filament::MaterialInstance*> getMaterialInstance(
      Material* material);

  // Disallow copy and assign.
  MaterialManager(const MaterialManager&) = delete;
  MaterialManager& operator=(const MaterialManager&) = delete;

 private:
  plugin_filament_view::CustomModelViewer* modelViewer_;
  const std::string& flutterAssetsPath_;

  std::unique_ptr<plugin_filament_view::MaterialLoader> materialLoader_;
  std::unique_ptr<plugin_filament_view::TextureLoader> textureLoader_;

  Resource<::filament::Material*> loadMaterial(Material* material);
  Resource<::filament::MaterialInstance*> setupMaterialInstance(
      ::filament::Material* materialResult);
};
}  // namespace plugin_filament_view