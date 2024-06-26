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

#include "model_loader.h"

#include <algorithm>  // for max
#include <sstream>

#include <filament/DebugRegistry.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/TextureProvider.h>
#include <math/mat4.h>
#include <math/vec3.h>
#include <asio/post.hpp>

#include "gltfio/materials/uberarchive.h"

#include "core/include/file_utils.h"
#include "plugins/common/curl_client/curl_client.h"

namespace plugin_filament_view {

using ::filament::gltfio::AssetConfiguration;
using ::filament::gltfio::AssetLoader;
using ::filament::gltfio::ResourceConfiguration;
using ::filament::gltfio::ResourceLoader;

ModelLoader::ModelLoader(CustomModelViewer* modelViewer)
    : modelViewer_(modelViewer), strand_(modelViewer->getStrandContext()) {
  SPDLOG_TRACE("++ModelLoader::ModelLoader");
  assert(modelViewer_);
  engine_ = modelViewer->getFilamentEngine();
  assetPath_ = modelViewer->getAssetPath();

  materialProvider_ = ::filament::gltfio::createUbershaderProvider(
      engine_, UBERARCHIVE_DEFAULT_DATA,
      static_cast<size_t>(UBERARCHIVE_DEFAULT_SIZE));
  SPDLOG_DEBUG("UbershaderProvider MaterialsCount: {}",
               materialProvider_->getMaterialsCount());

  AssetConfiguration assetConfiguration{};
  assetConfiguration.engine = engine_;
  assetConfiguration.materials = materialProvider_;
  assetLoader_ = AssetLoader::create(assetConfiguration);

  ResourceConfiguration resourceConfiguration{};
  resourceConfiguration.engine = engine_;
  resourceConfiguration.normalizeSkinningWeights = true;
  resourceLoader_ = new ResourceLoader(resourceConfiguration);
  auto decoder = filament::gltfio::createStbProvider(engine_);
  resourceLoader_->addTextureProvider("image/png", decoder);
  resourceLoader_->addTextureProvider("image/jpeg", decoder);

  assetPath_ = modelViewer->getAssetPath();
  SPDLOG_TRACE("--ModelLoader::ModelLoader");
}

ModelLoader::~ModelLoader() {
  delete resourceLoader_;
  resourceLoader_ = nullptr;

  if (assetLoader_) {
    AssetLoader::destroy(&assetLoader_);
  }
}

void ModelLoader::destroyModel() {
  // fetchResourcesJob?.cancel()
  resourceLoader_->asyncCancelLoad();
  resourceLoader_->evictResourceData();

  if (asset_) {
    modelViewer_->getFilamentScene()->removeEntities(asset_->getEntities(),
                                                     asset_->getEntityCount());
    assetLoader_->destroyAsset(asset_);
    asset_ = nullptr;
    modelViewer_->setAnimator(nullptr);
  }
}

/**
 * Loads a monolithic binary glb and populates the Filament scene.
 */
void ModelLoader::loadModelGlb(const std::vector<uint8_t>& buffer,
                               const ::filament::float3* centerPosition,
                               float scale,
                               bool autoScaleEnabled) {
  destroyModel();
  asset_ = assetLoader_->createAsset(buffer.data(),
                                     static_cast<uint32_t>(buffer.size()));
  if (!asset_) {
    return;
  }

  resourceLoader_->asyncBeginLoad(asset_);
  modelViewer_->setAnimator(asset_->getInstance()->getAnimator());
  asset_->releaseSourceData();
  if (autoScaleEnabled) {
    transformToUnitCube(centerPosition, scale);
  } else {
    clearRootTransform();
  }
}

void ModelLoader::loadModelGltf(
    const std::vector<uint8_t>& buffer,
    const ::filament::float3* centerPosition,
    float scale,
    std::function<const ::filament::backend::BufferDescriptor&(
        std::string uri)>& /* callback */,
    bool transform) {
  destroyModel();
  asset_ = assetLoader_->createAsset(buffer.data(),
                                     static_cast<uint32_t>(buffer.size()));
  if (!asset_) {
    return;
  }

  auto uri_data = asset_->getResourceUris();
  auto uris = std::vector<const char*>(
      uri_data, uri_data + asset_->getResourceUriCount());
  for (const auto uri : uris) {
    SPDLOG_DEBUG("resource uri: {}", uri);
#if 0   // TODO
              auto resourceBuffer = callback(uri);
              if (!resourceBuffer) {
                  this->asset_ = nullptr;
                  return;
              }
              resourceLoader_->addResourceData(uri, resourceBuffer);
#endif  // TODO
  }
  resourceLoader_->asyncBeginLoad(asset_);
  modelViewer_->setAnimator(asset_->getInstance()->getAnimator());
  asset_->releaseSourceData();
  if (transform) {
    transformToUnitCube(centerPosition, scale);
  }
}

void ModelLoader::transformToUnitCube(
    const ::filament::float3* /* centerPoint */,
    float /* modelScale */) {
  if (!asset_) {
    return;
  }
  auto aabb = asset_->getBoundingBox();
  auto center = aabb.center();
  auto halfExtent = aabb.extent();
  auto maxExtent = max(halfExtent) * 2;
  auto scaleFactor = 2.0f / maxExtent;
  auto transform = ::filament::math::mat4f::scaling(scaleFactor) *
                   ::filament::math::mat4f::translation(-center);
  auto& tm = engine_->getTransformManager();
  tm.setTransform(tm.getInstance(asset_->getRoot()), transform);
}

void ModelLoader::populateScene(::filament::gltfio::FilamentAsset* asset) {
  auto& rcm = engine_->getRenderableManager();

  size_t count = asset->popRenderables(nullptr, 0);
  while (count) {
    asset->popRenderables(readyRenderables_, count);
    for (int i = 0; i < count; i++) {
      auto ri = rcm.getInstance(readyRenderables_[i]);
      rcm.setScreenSpaceContactShadows(ri, true);
    }
    modelViewer_->getFilamentScene()->addEntities(readyRenderables_, count);
    count = asset->popRenderables(nullptr, 0);
  }
  auto lightEntities = asset->getLightEntities();
  if (lightEntities) {
    modelViewer_->getFilamentScene()->addEntities(asset->getLightEntities(),
                                                  sizeof(*lightEntities));
  }
}

void ModelLoader::updateScene() {
  // Allow the resource loader to finalize textures that have become ready.
  resourceLoader_->asyncUpdateLoad();

  // Add render-able entities to the scene as they become ready.
  if (asset_) {
    populateScene(asset_);
  }
}

void ModelLoader::removeAsset() {
  if (!isRemoteMode()) {
    modelViewer_->getFilamentScene()->removeEntities(asset_->getEntities(),
                                                     asset_->getEntityCount());
    asset_ = nullptr;
  }
}

std::optional<::filament::math::mat4f> ModelLoader::getModelTransform() {
  if (asset_) {
    auto root = asset_->getRoot();
    auto& tm = asset_->getEngine()->getTransformManager();
    auto instance = tm.getInstance(root);
    return tm.getTransform(instance);
  }
  return std::nullopt;
}

void ModelLoader::clearRootTransform() {
  auto root = asset_->getRoot();
  auto& tm = asset_->getEngine()->getTransformManager();
  auto instance = tm.getInstance(root);
  tm.setTransform(instance, ::filament::mat4f{});
}

std::future<Resource<std::string_view>> ModelLoader::loadGlbFromAsset(
    const std::string& path,
    float scale,
    const ::filament::math::float3* centerPosition,
    bool isFallback) {
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto promise_future(promise->get_future());
  modelViewer_->setModelState(ModelState::LOADING);
  asio::post(strand_, [&, promise, path, scale, centerPosition, isFallback] {
    auto buffer = readBinaryFile(path, assetPath_);
    handleFile(buffer, path, scale, centerPosition, isFallback, promise);
  });
  return promise_future;
}

std::future<Resource<std::string_view>> ModelLoader::loadGlbFromUrl(
    std::string url,
    float scale,
    const ::filament::math::float3* centerPosition,
    bool isFallback) {
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto promise_future(promise->get_future());
  modelViewer_->setModelState(ModelState::LOADING);
  asio::post(strand_, [&, promise, url = std::move(url), scale, centerPosition,
                       isFallback] {
    plugin_common_curl::CurlClient client;
    // TODO client.Init(url);
    auto buffer = client.RetrieveContentAsVector();
    if (client.GetCode() != CURLE_OK) {
      modelViewer_->setModelState(ModelState::ERROR);
      promise->set_value(
          Resource<std::string_view>::Error("Couldn't load Glb from " + url));
    }
    handleFile(buffer, url, scale, centerPosition, isFallback, promise);
  });
  return promise_future;
}

void ModelLoader::handleFile(
    const std::vector<uint8_t>& buffer,
    const std::string& fileSource,
    float scale,
    const ::filament::math::float3* centerPosition,
    bool isFallback,
    const std::shared_ptr<std::promise<Resource<std::string_view>>>& promise) {
  if (!buffer.empty()) {
    loadModelGlb(buffer, centerPosition, scale, true);
    modelViewer_->setModelState(isFallback ? ModelState::FALLBACK_LOADED
                                           : ModelState::LOADED);
    promise->set_value(Resource<std::string_view>::Success(
        "Loaded glb model successfully from " + fileSource));
  } else {
    modelViewer_->setModelState(ModelState::ERROR);
    promise->set_value(Resource<std::string_view>::Error(
        "Couldn't load glb model from " + fileSource));
  }
}

std::future<Resource<std::string_view>> ModelLoader::loadGltfFromAsset(
    const std::string& /* path */,
    const std::string& /* pre_path */,
    const std::string& /* post_path */,
    float /* scale */,
    const ::filament::math::float3* /* centerPosition */,
    bool /* isFallback */) {
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());
  promise->set_value(Resource<std::string_view>::Error("Not implemented yet"));
  return future;
}

std::future<Resource<std::string_view>> ModelLoader::loadGltfFromUrl(
    const std::string& /* url */,
    float /* scale */,
    const ::filament::math::float3* /* centerPosition */,
    bool /* isFallback */) {
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());
  promise->set_value(Resource<std::string_view>::Error("Not implemented yet"));
  return future;
}

}  // namespace plugin_filament_view
