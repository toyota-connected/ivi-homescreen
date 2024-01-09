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

#include "skybox_manager.h"

#include <filesystem>
#include <fstream>

#include <asio/post.hpp>

#include "core/include/color.h"
#include "core/utils/hdr_loader.h"
#include "plugins/common/common.h"

namespace plugin_filament_view {
SkyboxManager::SkyboxManager(CustomModelViewer* modelViewer,
                             IBLProfiler* ibl_profiler,
                             const std::string& flutter_assets_path)
    : modelViewer_(modelViewer),
      engine_(modelViewer->getFilamentEngine()),
      ibl_profiler_(ibl_profiler),
      flutterAssetsPath_(flutter_assets_path) {
  SPDLOG_TRACE("++SkyboxManager::SkyboxManager");
  auto f = Initialize();
  f.wait();
  SPDLOG_TRACE("--SkyboxManager::SkyboxManager");
}

void SkyboxManager::destroySkybox() {}

std::future<void> SkyboxManager::Initialize() {
  const auto promise(std::make_shared<std::promise<void>>());
  auto future(promise->get_future());

  asio::post(modelViewer_->getStrandContext(), [&, promise] {
    auto whiteSkybox = ::filament::Skybox::Builder()
                           .color({1.0f, 1.0f, 1.0f, 1.0f})
                           .build(*engine_);
    modelViewer_->getFilamentScene()->setSkybox(whiteSkybox);
  });

  return future;
}

void SkyboxManager::setDefaultSkybox() {
  SPDLOG_TRACE("++SkyboxManager::setDefaultSkybox");
  modelViewer_->setSkyboxState(SceneState::LOADING);
  setTransparentSkybox();
  modelViewer_->setSkyboxState(SceneState::LOADED);
  SPDLOG_TRACE("--SkyboxManager::setDefaultSkybox");
}

void SkyboxManager::setTransparentSkybox() {
  modelViewer_->getFilamentScene()->setSkybox(nullptr);
}

std::future<Resource<std::string_view>> SkyboxManager::setSkyboxFromHdrAsset(
    const std::string& path,
    bool showSun,
    bool shouldUpdateLight,
    float intensity) {
  SPDLOG_TRACE("++SkyboxManager::setSkyboxFromHdrAsset");
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());

  modelViewer_->setSkyboxState(SceneState::LOADING);
  std::filesystem::path asset_path = modelViewer_->getAssetPath();
  asset_path /= path;
  if (path.empty() || !std::filesystem::exists(asset_path)) {
    modelViewer_->setSkyboxState(SceneState::ERROR);
    promise->set_value(
        Resource<std::string_view>::Error("Skybox Asset path is not valid"));
  }

  asio::post(modelViewer_->getStrandContext(),
             [&, promise, asset_path, showSun, shouldUpdateLight, intensity] {
               promise->set_value(loadSkyboxFromHdrFile(
                   asset_path, showSun, shouldUpdateLight, intensity));
             });

  SPDLOG_TRACE("--SkyboxManager::setSkyboxFromHdrAsset");
  return future;
}

std::future<Resource<std::string_view>> SkyboxManager::setSkyboxFromHdrUrl(
    const std::string& url,
    bool showSun,
    bool shouldUpdateLight,
    float intensity) {
  SPDLOG_TRACE("++SkyboxManager::setSkyboxFromHdrUrl");
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());

  modelViewer_->setSkyboxState(SceneState::LOADING);

  if (url.empty()) {
    modelViewer_->setSkyboxState(SceneState::ERROR);
    promise->set_value(Resource<std::string_view>::Error("URL is empty"));
    return future;
  }

  SPDLOG_DEBUG("Skybox downloading HDR Asset: {}", url.c_str());
  asio::post(modelViewer_->getStrandContext(),
             [&, promise, url, showSun, shouldUpdateLight, intensity] {
               plugin_common::CurlClient client;
               client.Init(url, {}, {});
               auto buffer = client.RetrieveContentAsVector();
               if (client.GetCode() != CURLE_OK) {
                 modelViewer_->setSkyboxState(SceneState::ERROR);
                 promise->set_value(Resource<std::string_view>::Error(
                     "Couldn't load skybox from " + url));
               }
               if (!buffer.empty()) {
                 promise->set_value(loadSkyboxFromHdrBuffer(
                     buffer, showSun, shouldUpdateLight, intensity));
               } else {
                 modelViewer_->setSkyboxState(SceneState::ERROR);
                 promise->set_value(Resource<std::string_view>::Error(
                     "Couldn't load HDR file from " + url));
               }
             });
  SPDLOG_TRACE("--SkyboxManager::setSkyboxFromHdrUrl");
  return future;
}

std::future<Resource<std::string_view>> SkyboxManager::setSkyboxFromKTXAsset(
    const std::string& path) {
  SPDLOG_TRACE("++SkyboxManager::setSkyboxFromKTXAsset");
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());

  modelViewer_->setSkyboxState(SceneState::LOADING);
  std::filesystem::path asset_path = modelViewer_->getAssetPath();
  asset_path /= path;
  if (path.empty() || !std::filesystem::exists(asset_path)) {
    modelViewer_->setSkyboxState(SceneState::ERROR);
    promise->set_value(
        Resource<std::string_view>::Error("KTX Asset path is not valid"));
  }

  SPDLOG_DEBUG("Skybox loading KTX Asset: {}", asset_path.c_str());
  asio::post(modelViewer_->getStrandContext(), [&, promise, asset_path] {
    std::ifstream stream(asset_path, std::ios::in | std::ios::binary);
    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(stream)),
                                std::istreambuf_iterator<char>());
    if (!buffer.empty()) {
#if 0  // TODO
                auto skybox = KTX1Loader.createSkybox(*engine, buffer);
                modelViewer_->destroySkybox();
                modelViewer_->getFilamentScene()->setSkybox(skybox);
                modelViewer_->setSkyboxState(SceneState::LOADED);
#endif
      std::stringstream ss;
      ss << "Loaded environment successfully from " << asset_path;
      promise->set_value(Resource<std::string_view>::Success(ss.str()));
    } else {
      modelViewer_->setSkyboxState(SceneState::ERROR);
      std::stringstream ss;
      ss << "Couldn't change environment from " << asset_path;
      promise->set_value(Resource<std::string_view>::Error(ss.str()));
    }
  });
  SPDLOG_TRACE("--SkyboxManager::setSkyboxFromKTXAsset");
  return future;
}

std::future<Resource<std::string_view>> SkyboxManager::setSkyboxFromKTXUrl(
    const std::string& url) {
  SPDLOG_TRACE("++SkyboxManager::setSkyboxFromKTXUrl");
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());
  modelViewer_->setSkyboxState(SceneState::LOADING);
  if (url.empty()) {
    modelViewer_->setSkyboxState(SceneState::ERROR);
    promise->set_value(Resource<std::string_view>::Error("URL is empty"));
    return future;
  }

  asio::post(modelViewer_->getStrandContext(), [&, promise, url] {
    plugin_common::CurlClient client;
    client.Init(url, {}, {});
    auto buffer = client.RetrieveContentAsVector();
    if (client.GetCode() != CURLE_OK) {
      modelViewer_->setSkyboxState(SceneState::ERROR);
      std::stringstream ss;
      ss << "Couldn't load skybox from " << url;
      promise->set_value(Resource<std::string_view>::Error(ss.str()));
    }

    if (!buffer.empty()) {
#if 0  // TODO
                auto skybox = KTX1Loader.createSkybox(*engine, buffer);
                modelViewer_->destroySkybox();
                modelViewer_->getFilamentScene()->setSkybox(skybox);
                modelViewer_->setSkyboxState(SceneState::LOADED);
#endif
      std::stringstream ss;
      ss << "Loaded skybox successfully from " << url;
      promise->set_value(Resource<std::string_view>::Success(ss.str()));
    } else {
      modelViewer_->setSkyboxState(SceneState::ERROR);
      std::stringstream ss;
      ss << "Couldn't load skybox from " << url;
      promise->set_value(Resource<std::string_view>::Error(ss.str()));
    }
  });

  SPDLOG_TRACE("--SkyboxManager::setSkyboxFromKTXUrl");
  return future;
}

std::future<Resource<std::string_view>> SkyboxManager::setSkyboxFromColor(
    const std::string& color) {
  SPDLOG_TRACE("++SkyboxManager::setSkyboxFromColor");
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());

  if (color.empty()) {
    modelViewer_->setSkyboxState(SceneState::ERROR);
    promise->set_value(Resource<std::string_view>::Error("Color is Invalid"));
    return future;
  }
  asio::post(modelViewer_->getStrandContext(), [&, promise, color] {
    modelViewer_->setSkyboxState(SceneState::LOADING);
    auto colorArray = colorOf(color);
    auto skybox =
        ::filament::Skybox::Builder().color(colorArray).build(*engine_);
    modelViewer_->getFilamentScene()->setSkybox(skybox);
    modelViewer_->setSkyboxState(SceneState::LOADED);
    promise->set_value(Resource<std::string_view>::Success(
        "Loaded environment successfully from color"));
  });

  SPDLOG_TRACE("--SkyboxManager::setSkyboxFromColor");
  return future;
}

Resource<std::string_view> SkyboxManager::loadSkyboxFromHdrFile(
    const std::string& assetPath,
    bool showSun,
    bool shouldUpdateLight,
    float intensity) {
  ::filament::Texture* texture;
  try {
    texture = HDRLoader::createTexture(engine_, assetPath);
  } catch (...) {
    modelViewer_->setSkyboxState(SceneState::ERROR);
    return Resource<std::string_view>::Error("Could not decode HDR buffer");
  }
  if (texture) {
    auto skyboxTexture = ibl_profiler_->createCubeMapTexture(texture);
    engine_->destroy(texture);

    if (skyboxTexture) {
      auto sky = ::filament::Skybox::Builder()
                     .environment(skyboxTexture)
                     .showSun(showSun)
                     .build(*engine_);

      // updates scene light with skybox when loaded with the same hdr file
      if (shouldUpdateLight) {
        auto reflections = ibl_profiler_->getLightReflection(skyboxTexture);
        auto ibl = ::filament::IndirectLight::Builder()
                       .reflections(reflections)
                       .intensity(intensity)
                       .build(*engine_);
        // destroy the previous IBl
        auto indirectLight =
            modelViewer_->getFilamentScene()->getIndirectLight();
        engine_->destroy(indirectLight);
        modelViewer_->getFilamentScene()->setIndirectLight(ibl);
      }

      modelViewer_->destroySkybox();
      modelViewer_->getFilamentScene()->setSkybox(sky);
    }
    modelViewer_->setSkyboxState(SceneState::LOADED);
    return Resource<std::string_view>::Success(
        "Loaded hdr skybox successfully");
  } else {
    modelViewer_->setSkyboxState(SceneState::ERROR);
    return Resource<std::string_view>::Error("Could not decode HDR file");
  }
}

Resource<std::string_view> SkyboxManager::loadSkyboxFromHdrBuffer(
    const std::vector<uint8_t>& buffer,
    bool showSun,
    bool shouldUpdateLight,
    float intensity) {
  ::filament::Texture* texture;
  try {
    texture = HDRLoader::createTexture(engine_, buffer);
  } catch (...) {
    modelViewer_->setSkyboxState(SceneState::ERROR);
    return Resource<std::string_view>::Error("Could not decode HDR buffer");
  }
  if (texture) {
    auto skyboxTexture = ibl_profiler_->createCubeMapTexture(texture);
    engine_->destroy(texture);

    if (skyboxTexture) {
      auto sky = ::filament::Skybox::Builder()
                     .environment(skyboxTexture)
                     .showSun(showSun)
                     .build(*engine_);

      // updates scene light with skybox when loaded with the same hdr file
      if (shouldUpdateLight) {
        auto reflections = ibl_profiler_->getLightReflection(skyboxTexture);
        auto ibl = ::filament::IndirectLight::Builder()
                       .reflections(reflections)
                       .intensity(intensity)
                       .build(*engine_);
        // destroy the previous IBl
        auto indirectLight =
            modelViewer_->getFilamentScene()->getIndirectLight();
        engine_->destroy(indirectLight);
        modelViewer_->getFilamentScene()->setIndirectLight(ibl);
      }

      modelViewer_->destroySkybox();
      modelViewer_->getFilamentScene()->setSkybox(sky);
    }
    modelViewer_->setSkyboxState(SceneState::LOADED);
    return Resource<std::string_view>::Success(
        "Loaded hdr skybox successfully");
  } else {
    modelViewer_->setSkyboxState(SceneState::ERROR);
    return Resource<std::string_view>::Error("Could not decode HDR file");
  }
}

}  // namespace plugin_filament_view