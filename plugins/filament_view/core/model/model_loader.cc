
#include "model_loader.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <filament/DebugRegistry.h>
#include <filament/LightManager.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <math/mat4.h>
#include <math/vec3.h>
#include "asio/post.hpp"

#include "generated/resources/gltf_demo.h"
#include "materials/uberarchive.h"

#include "logging/logging.h"

namespace plugin_filament_view {

using ::filament::gltfio::AssetConfiguration;
using ::filament::gltfio::AssetLoader;
using ::filament::gltfio::ResourceConfiguration;
using ::filament::gltfio::ResourceLoader;

ModelLoader::ModelLoader(CustomModelViewer* modelViewer)
    : engine_(modelViewer->getFilamentEngine()), modelViewer_(modelViewer) {
  SPDLOG_TRACE("++ModelLoader::ModelLoader");
  assert(modelViewer_);
  auto engine = modelViewer->getFilamentEngine();
  materialProvider_ = ::filament::gltfio::createUbershaderProvider(
      engine, UBERARCHIVE_DEFAULT_DATA,
      static_cast<size_t>(UBERARCHIVE_DEFAULT_SIZE));
  SPDLOG_DEBUG("UbershaderProvider MaterialsCount: {}",
               materialProvider_->getMaterialsCount());

  AssetConfiguration assetConfiguration{};
  assetConfiguration.engine = engine;
  assetConfiguration.materials = materialProvider_;
  assetLoader_ = AssetLoader::create(assetConfiguration);

  ResourceConfiguration resourceConfiguration{};
  resourceConfiguration.engine = engine;
  resourceConfiguration.normalizeSkinningWeights = true;
  resourceLoader_ = new ResourceLoader(resourceConfiguration);

  assetPath_ = modelViewer->getAssetPath();
  visibleScenes_.reset();
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
                               const Position* centerPosition,
                               float scale,
                               bool transform) {
  destroyModel();
  asset_ = assetLoader_->createAsset(buffer.data(),
                                     static_cast<uint32_t>(buffer.size()));
  resourceLoader_->asyncBeginLoad(asset_);
  modelViewer_->setAnimator(asset_->getInstance()->getAnimator());
  asset_->releaseSourceData();
  if (transform) {
    transformToUnitCube(centerPosition, scale);
  }
}

void ModelLoader::loadModelGltf(
    const std::vector<uint8_t>& buffer,
    const Position* centerPosition,
    float scale,
    std::function<const ::filament::backend::BufferDescriptor&(
        std::string uri)>& callback,
    bool transform) {
  destroyModel();
  asset_ = assetLoader_->createAsset(buffer.data(),
                                     static_cast<uint32_t>(buffer.size()));
  if (asset_) {
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
}

filament::math::mat4f inline fitIntoUnitCube(const ::filament::Aabb& bounds,
                                             float zoffset) {
  filament::math::float3 minpt = bounds.min;
  filament::math::float3 maxpt = bounds.max;
  float maxExtent;
  maxExtent = std::max(maxpt.x - minpt.x, maxpt.y - minpt.y);
  maxExtent = std::max(maxExtent, maxpt.z - minpt.z);
  float scaleFactor = 2.0f / maxExtent;
  filament::math::float3 center = (minpt + maxpt) / 2.0f;
  center.z += zoffset / scaleFactor;
  return filament::math::mat4f::scaling(filament::math::float3(scaleFactor)) *
         filament::math::mat4f::translation(-center);
}

void ModelLoader::transformToUnitCube(const Position* centerPoint,
                                      float scale) {
  ::filament::float3 centerPosition;
  if (!centerPoint) {
    centerPosition = CustomModelViewer::kDefaultObjectPosition;
  } else {
    centerPosition = centerPoint->toFloatArray();
  }

  auto& tm = engine_->getTransformManager();
  auto root = tm.getInstance(asset_->getRoot());
  ::filament::mat4f transform;
  if (settings_.viewer.autoScaleEnabled) {
    auto* instance = asset_->getInstance();
    ::filament::Aabb aabb =
        instance ? instance->getBoundingBox() : asset_->getBoundingBox();
    transform = fitIntoUnitCube(aabb, 4);
  }
  tm.setTransform(root, transform);
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

const ::filament::math::mat4f& ModelLoader::getModelTransform() {
  auto root = asset_->getRoot();
  auto& tm = asset_->getEngine()->getTransformManager();
  auto instance = tm.getInstance(root);
  return tm.getTransform(instance);
}
}  // namespace plugin_filament_view
