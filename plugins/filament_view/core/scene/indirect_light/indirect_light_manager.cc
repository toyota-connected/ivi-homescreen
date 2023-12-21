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
#include "logging/logging.h"
#include "textures/texture.h"

namespace plugin_filament_view {
IndirectLightManager::IndirectLightManager(CustomModelViewer* model_viewer,
                                           IBLProfiler* ibl_profiler)
    : model_viewer_(model_viewer),
      ibl_prefilter_(ibl_profiler),
      engine_(model_viewer->getFilamentEngine()) {
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
  SPDLOG_TRACE("--IndirectLightManager::setDefaultIndirectLight");
};

std::future<std::string> IndirectLightManager::setIndirectLight(
    DefaultIndirectLight* indirectLight) {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());
  model_viewer_->setLightState(SceneState::LOADING);
  if (!indirectLight) {
    model_viewer_->setLightState(SceneState::ERROR);
    promise->set_value("Light is null");
    return future;
  }
  asio::post(model_viewer_->getStrandContext(), [&, promise, indirectLight] {
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

    model_viewer_->setLightState(SceneState::LOADED);
    promise->set_value("changed Light successfully");
  });
  return future;
}

std::future<std::string> IndirectLightManager::setIndirectLightFromKtxAsset(
    std::string path,
    double intensity) {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());
  asio::post(model_viewer_->getStrandContext(),
             [&, promise, path = std::move(path), intensity] {
               promise->set_value("");
             });
  return future;
}

std::future<std::string> IndirectLightManager::setIndirectLightFromKtxUrl(
    std::string url,
    double intensity) {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());
  asio::post(model_viewer_->getStrandContext(),
             [&, promise, url = std::move(url), intensity] {
               promise->set_value("");
             });
  return future;
}

std::string IndirectLightManager::loadIndirectLightHdrFromFile(
    const std::string& asset_path,
    double intensity) {
  model_viewer_->setLightState(SceneState::LOADING);

  ::filament::Texture* texture;
  try {
    texture = HDRLoader::createTexture(engine_, asset_path);
  } catch (...) {
    model_viewer_->setLightState(SceneState::ERROR);
    return "Could not decode HDR file";
  }
  auto skyboxTexture = ibl_prefilter_->createCubeMapTexture(texture);
  engine_->destroy(texture);

  auto reflections = ibl_prefilter_->getLightReflection(skyboxTexture);

  auto ibl = ::filament::IndirectLight::Builder()
                 .reflections(reflections)
                 .intensity(static_cast<float>(intensity))
                 .build(*engine_);

  // destroy the previous IBl
  model_viewer_->destroyIndirectLight();

  model_viewer_->getFilamentView()->getScene()->setIndirectLight(ibl);
  model_viewer_->setLightState(SceneState::LOADED);

  return "loaded Indirect light successfully";
}

std::future<std::string> IndirectLightManager::setIndirectLightFromHdrAsset(
    std::string path,
    double intensity) {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());
  model_viewer_->setLightState(SceneState::LOADING);
  asio::post(model_viewer_->getStrandContext(),
             [&, promise, path = std::move(path), intensity] {
               std::filesystem::path asset_path(model_viewer_->getAssetPath());
               asset_path /= path;
               if (path.empty() || !std::filesystem::exists(asset_path)) {
                 model_viewer_->setModelState(ModelState::ERROR);
                 promise->set_value("Asset path not valid");
               }
               try {
                 promise->set_value(loadIndirectLightHdrFromFile(
                     asset_path.c_str(), intensity));
               } catch (...) {
                 model_viewer_->setLightState(SceneState::ERROR);
                 promise->set_value("Couldn't changed Light from asset");
               }
             });
  return future;
}

std::future<std::string> IndirectLightManager::setIndirectLightFromHdrUrl(
    std::string url,
    double intensity) {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());
  asio::post(model_viewer_->getStrandContext(),
             [&, promise, url = std::move(url), intensity] {
               promise->set_value("");
             });
  return future;
}

}  // namespace plugin_filament_view