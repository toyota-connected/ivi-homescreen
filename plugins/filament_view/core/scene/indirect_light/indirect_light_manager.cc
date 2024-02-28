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

#include "indirect_light_manager.h"

#include <filesystem>
#include <memory>

#include <filament/Texture.h>
#include <asio/post.hpp>
#include <utility>

#include "core/utils/hdr_loader.h"
#include "plugins/common/common.h"

namespace plugin_filament_view {
IndirectLightManager::IndirectLightManager(CustomModelViewer* modelViewer,
                                           IBLProfiler* ibl_profiler)
    : modelViewer_(modelViewer),
      ibl_prefilter_(ibl_profiler),
      engine_(modelViewer->getFilamentEngine()) {
  SPDLOG_TRACE("++IndirectLightManager::IndirectLightManager");
  setDefaultIndirectLight();
  SPDLOG_TRACE("--IndirectLightManager::IndirectLightManager");
}

void IndirectLightManager::setDefaultIndirectLight() {
  SPDLOG_TRACE("++IndirectLightManager::setDefaultIndirectLight");
  auto light = std::make_unique<DefaultIndirectLight>();
  auto f = setIndirectLight(light.get());
  f.wait();
  light.reset();
  SPDLOG_TRACE("--IndirectLightManager::setDefaultIndirectLight: {}",
               f.get().getMessage());
};

std::future<Resource<std::string_view>> IndirectLightManager::setIndirectLight(
    DefaultIndirectLight* indirectLight) {
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());
  modelViewer_->setLightState(SceneState::LOADING);
  if (!indirectLight) {
    modelViewer_->setLightState(SceneState::ERROR);
    promise->set_value(Resource<std::string_view>::Error("Light is null"));
    return future;
  }
  asio::post(modelViewer_->getStrandContext(), [&, promise, indirectLight] {
    auto builder = ::filament::IndirectLight::Builder();
    builder.intensity(indirectLight->getIntensity());
    builder.radiance(static_cast<uint8_t>(indirectLight->radiance_.size()),
                     indirectLight->radiance_.data());
    builder.irradiance(static_cast<uint8_t>(indirectLight->irradiance_.size()),
                       indirectLight->irradiance_.data());
    if (indirectLight->rotation_.has_value()) {
      builder.rotation(indirectLight->rotation_.value());
    }
    builder.build(*engine_);

    modelViewer_->setLightState(SceneState::LOADED);
    promise->set_value(
        Resource<std::string_view>::Success("changed Light successfully"));
  });
  return future;
}

std::future<Resource<std::string_view>>
IndirectLightManager::setIndirectLightFromKtxAsset(std::string path,
                                                   double intensity) {
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());
  asio::post(modelViewer_->getStrandContext(),
             [&, promise, path = std::move(path), intensity] {
               promise->set_value(
                   Resource<std::string_view>::Error("Not implemented"));
             });
  return future;
}

std::future<Resource<std::string_view>>
IndirectLightManager::setIndirectLightFromKtxUrl(std::string url,
                                                 double intensity) {
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());
  asio::post(modelViewer_->getStrandContext(),
             [&, promise, url = std::move(url), intensity] {
               promise->set_value(
                   Resource<std::string_view>::Error("Not implemented"));
             });
  return future;
}

Resource<std::string_view> IndirectLightManager::loadIndirectLightHdrFromFile(
    const std::string& asset_path,
    double intensity) {
  modelViewer_->setLightState(SceneState::LOADING);

  ::filament::Texture* texture;
  try {
    texture = HDRLoader::createTexture(engine_, asset_path);
  } catch (...) {
    modelViewer_->setLightState(SceneState::ERROR);
    return Resource<std::string_view>::Error("Could not decode HDR file");
  }
  auto skyboxTexture = ibl_prefilter_->createCubeMapTexture(texture);
  engine_->destroy(texture);

  auto reflections = ibl_prefilter_->getLightReflection(skyboxTexture);

  auto ibl = ::filament::IndirectLight::Builder()
                 .reflections(reflections)
                 .intensity(static_cast<float>(intensity))
                 .build(*engine_);

  // destroy the previous IBl
  modelViewer_->destroyIndirectLight();

  modelViewer_->getFilamentView()->getScene()->setIndirectLight(ibl);
  modelViewer_->setLightState(SceneState::LOADED);

  return Resource<std::string_view>::Success(
      "loaded Indirect light successfully");
}

std::future<Resource<std::string_view>>
IndirectLightManager::setIndirectLightFromHdrAsset(std::string path,
                                                   double intensity) {
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());
  modelViewer_->setLightState(SceneState::LOADING);
  asio::post(modelViewer_->getStrandContext(),
             [&, promise, path = std::move(path), intensity] {
               std::filesystem::path asset_path(modelViewer_->getAssetPath());
               asset_path /= path;
               if (path.empty() || !std::filesystem::exists(asset_path)) {
                 modelViewer_->setModelState(ModelState::ERROR);
                 promise->set_value(
                     Resource<std::string_view>::Error("Asset path not valid"));
               }
               try {
                 promise->set_value(loadIndirectLightHdrFromFile(
                     asset_path.c_str(), intensity));
               } catch (...) {
                 modelViewer_->setLightState(SceneState::ERROR);
                 promise->set_value(Resource<std::string_view>::Error(
                     "Couldn't changed Light from asset"));
               }
             });
  return future;
}

std::future<Resource<std::string_view>>
IndirectLightManager::setIndirectLightFromHdrUrl(std::string url,
                                                 double intensity) {
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());
  asio::post(modelViewer_->getStrandContext(),
             [&, promise, url = std::move(url), intensity] {
               promise->set_value(
                   Resource<std::string_view>::Error("Not implemented"));
             });
  return future;
}

}  // namespace plugin_filament_view