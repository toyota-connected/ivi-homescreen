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
#include <fstream>
#include <memory>
#include <sstream>

#include <asio/post.hpp>
#include <utility>

#include "logging/logging.h"

namespace plugin_filament_view {
IndirectLightManager::IndirectLightManager(
    CustomModelViewer* model_viewer)
    : model_viewer_(model_viewer),
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
  asio::post(*model_viewer_->getStrandContext(), [&, promise, indirectLight] {
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
  asio::post(*model_viewer_->getStrandContext(),
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
  asio::post(*model_viewer_->getStrandContext(),
             [&, promise, url = std::move(url), intensity] {
               promise->set_value("");
             });
  return future;
}

std::string IndirectLightManager::loadIndirectLightHdrFromBuffer(
    const std::vector<uint8_t>& buffer,
    double intensity) {
  model_viewer_->setLightState(SceneState::LOADING);
#if 0
  try {
    auto texture = HDRLoader.createTexture(engine_, buffer);
    if (!texture) {
      model_viewer_->setLightState(SceneState::ERROR);
      return "Could not decode HDR file";
    } else {
      auto skyboxTexture = iblPrefilter->createCubeMapTexture(texture);

      engine_->destroyTexture(texture);

      auto reflections = iblPrefilter->getLightReflection(skyboxTexture);

      auto ibl =
          ::filament::IndirectLight::Builder()
              .reflections(reflections)
              .intensity(intensity.has_value() ? intensity.value() : 30'000.f)
              .build(engine_);

      // destroy the previous IBl
      model_viewer_->destroyIndirectLight();

      // model_viewer_->getScene().scene.indirectLight = ibl
      model_viewer_->setLightState(SceneState::LOADED);

      return "loaded Indirect light successfully";
    }
  } catch (...) {
    model_viewer_->setLightState(SceneState::ERROR);
    return "Could not decode HDR file";
  }
#endif  // TODO
  model_viewer_->setLightState(SceneState::LOADED);
  return "loaded Indirect light successfully";
}

std::future<std::string> IndirectLightManager::setIndirectLightFromHdrAsset(
    std::string path,
    double intensity) {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());
  model_viewer_->setLightState(SceneState::LOADING);
  asio::post(
      *model_viewer_->getStrandContext(),
      [&, promise, path = std::move(path), intensity] {
        std::filesystem::path asset_path(model_viewer_->getAssetPath());
        asset_path /= path;
        if (path.empty() || !std::filesystem::exists(asset_path)) {
          model_viewer_->setModelState(ModelState::ERROR);
          promise->set_value("Asset path not valid");
        }
        try {
          SPDLOG_DEBUG("Attempting to load {}", asset_path.c_str());
          std::ifstream stream(asset_path, std::ios::in | std::ios::binary);
          std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(stream)),
                                      std::istreambuf_iterator<char>());
          SPDLOG_DEBUG("Loaded: {}", asset_path.c_str());
          promise->set_value(loadIndirectLightHdrFromBuffer(buffer, intensity));
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
  asio::post(*model_viewer_->getStrandContext(),
             [&, promise, url = std::move(url), intensity] {
               promise->set_value("");
             });
  return future;
}

}  // namespace plugin_filament_view