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

#include "core/scene/material/material_manager.h"

#include "core/scene/material/utils/material_instance.h"
#include "logging/logging.h"

namespace plugin_filament_view {

MaterialManager::MaterialManager(CustomModelViewer* modelViewer,
                                 const std::string& flutter_assets_path)
    : modelViewer_(modelViewer),
      flutterAssetsPath_(flutter_assets_path),
      materialLoader_(
          std::make_unique<MaterialLoader>(modelViewer, flutter_assets_path)),
      textureLoader_(
          std::make_unique<TextureLoader>(modelViewer, flutter_assets_path)) {
  SPDLOG_TRACE("++MaterialManager::MaterialManager");
  SPDLOG_TRACE("--MaterialManager::MaterialManager");
}

::filament::Material* MaterialManager::loadMaterial(Material* material) {
  // The Future object for loading Material
  if (!material->assetPath_.empty()) {
    return materialLoader_->loadMaterialFromAsset(material->assetPath_);
  } else if (!material->url_.empty()) {
    return materialLoader_->loadMaterialFromUrl(material->url_);
  } else {
    spdlog::error("You must provide material asset path or url");
    return nullptr;
  }
}

MaterialInstance* MaterialManager::setupMaterialInstance(
    ::filament::Material* materialResult) {
  if (!materialResult)
    return nullptr;

  auto count = materialResult->getParameterCount();
  std::vector<::filament::Material::ParameterInfo> parameters(count);

  auto actual = materialResult->getParameters(parameters.data(), count);
  assert(count == actual && actual == parameters.size());

  auto materialInstance =
      static_cast<MaterialInstance*>(materialResult->createInstance());

  for (const auto& param : parameters) {
    if (param.name) {
      spdlog::info("[Material] name: {}", param.name);

      // materialInstance->setParameter<TextureLoader>(param.name,
      // textureLoader_.get());
      materialInstance->setParameter(param, textureLoader_.get());
    }
  }

  return nullptr;
}

::filament::MaterialInstance* MaterialManager::getMaterialInstance(
    Material* material) {
  SPDLOG_TRACE("++MaterialManager::getMaterialInstance");

  if (!material) {
    spdlog::error("Material not found");
    return nullptr;
  }

  ::filament::Material* materialResult = loadMaterial(material);

  if (!materialResult) {
    SPDLOG_TRACE("--MaterialManager::getMaterialInstance");
    return nullptr;
  }

  MaterialInstance* materialInstance = setupMaterialInstance(materialResult);

  SPDLOG_TRACE("--MaterialManager::getMaterialInstance");
  return materialInstance;
}

}  // namespace plugin_filament_view