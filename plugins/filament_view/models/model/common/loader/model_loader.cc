
#include "model_loader.h"

#include <filament/TransformManager.h>
#include <filament/RenderableManager.h>

#include "logging/logging.h"

namespace plugin_filament_view {

ModelLoader::ModelLoader(CustomModelViewer* modelViewer,
                         ::filament::Engine* engine,
                         ::filament::gltfio::AssetLoader* assetLoader,
                         ::filament::gltfio::ResourceLoader* resourceLoader,
                         asio::io_context::strand* strand)
    : engine_(engine),
      assetLoader_(assetLoader),
      resourceLoader_(resourceLoader),
      strand_(strand),
      modelViewer_(modelViewer) {
  SPDLOG_TRACE("++ModelLoader::ModelLoader");
  assert(engine);
  assert(assetLoader);
  assert(resourceLoader);
  assert(strand_);
  assert(modelViewer_);
  visibleScenes_.reset();
  SPDLOG_TRACE("--ModelLoader::ModelLoader");
}

ModelLoader::~ModelLoader() = default;

/**
 * Loads a monolithic binary glTF and populates the Filament scene.
 */
void ModelLoader::loadModelGlb(const std::vector<uint8_t>& buffer,
                               const Position* centerPosition,
                               float scale,
                               bool transform) {
  // TODO destroyModel();

  spdlog::debug("thread_id: 0x{:x}", pthread_self());
  asset_ = assetLoader_->createAsset(buffer.data(), buffer.size());
  resourceLoader_->asyncBeginLoad(asset_);
  modelViewer_->setAnimator(asset_->getInstance()->getAnimator());
  asset_->releaseSourceData();
  if (transform) {
    transformToUnitCube(centerPosition, scale);
  }
  visibleScenes_.set(1);
}

void ModelLoader::loadModelGltf(const std::vector<uint8_t>& buffer,
                                const Position* centerPosition,
                                float scale,
                                bool transform) {
  // TODO destroyModel();

  spdlog::debug("thread_id: 0x{:x}", pthread_self());
  asset_ = assetLoader_->createAsset(buffer.data(), buffer.size());
  resourceLoader_->asyncBeginLoad(asset_);
  modelViewer_->setAnimator(asset_->getInstance()->getAnimator());
  //TODO asset_->releaseSourceData();
  if (transform) {
    transformToUnitCube(centerPosition, scale);
  }
  visibleScenes_.set(1);
}

void ModelLoader::loadModelGltfAsync(
    uint8_t* buffer,
    std::optional<float> scale,
    std::optional<Position>,
    const std::function<uint8_t*(std::string)> callback,
    bool transformToUnitCube) {
  assert(false);
}

void ModelLoader::transformToUnitCube(const Position* centerPoint,
                                      float scale) {
  // var center = asset.boundingBox.center.let { v -> Float3(v[0], v[1], v[2]) }
  // val halfExtent = asset.boundingBox.halfExtent.let { v -> Float3(v[0], v[1],
  // v[2]) } val maxExtent = 2.0f * max(halfExtent) val scaleFactor = 2.0f
  // *modelScale  / maxExtent center -= centerPosition / scaleFactor val
  // transform = scale(Float3(scaleFactor)) * translation(-center)
  // tm.setTransform(tm.getInstance(asset.root),
  // transpose(transform).toFloatArray())
}

void ModelLoader::populateScene(::filament::gltfio::FilamentAsset* asset) {
  if (!asset)
    return;

  while (size_t numWritten = asset->popRenderables(readyRenderables_, sizeof(readyRenderables_))) {
    asset->addEntitiesToScene(*modelViewer_->getScene(), readyRenderables_, numWritten, visibleScenes_);
  }
#if 0
  auto popRenderables = { count = asset->popRenderables(readyRenderables_); count != 0 }
  while (popRenderables()) {
    for (i in 0 until count) {
      auto ri = rcm.getInstance(readyRenderables[i]);
      rcm.setScreenSpaceContactShadows(ri, true)
    }
    modelViewer.scene.addEntities(readyRenderables.take(count).toIntArray());
  }
  modelViewer.scene.addEntities(asset.lightEntities);
#endif
}

void ModelLoader::updateScene() {
  // Allow the resource loader to finalize textures that have become ready.
  resourceLoader_->asyncUpdateLoad();

  // Add render-able entities to the scene as they become ready.
  populateScene(asset_);
}

}  // namespace plugin_filament_view
