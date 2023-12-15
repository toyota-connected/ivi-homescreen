
#include "model_loader.h"

namespace plugin_filament_view {

ModelLoader::ModelLoader(CustomModelViewer* modelViewer,
                         ::filament::gltfio::AssetLoader* assetLoader,
                         ::filament::gltfio::ResourceLoader* resourceLoader)
    : assetLoader_(assetLoader), resourceLoader_(resourceLoader) {
  engine_ = modelViewer->getEngine();
}

ModelLoader::~ModelLoader() {}

/**
 * Loads a monolithic binary glTF and populates the Filament scene.
 */
void ModelLoader::loadModelGlb(uint8_t* buffer,
                               std::optional<Position> centerPosition,
                               std::optional<float> scale,
                               bool transformToUnitCube) {
#if 0
  withContext(Dispatchers.Main) {
    destroyModel()
    asset = assetLoader.createAsset(buffer)
    asset?.let { asset ->
      resourceLoader.asyncBeginLoad(asset)
      modelViewer.animator = asset.instance.animator
      asset.releaseSourceData()
      if (transformToUnitCube) {
        transformToUnitCube(centerPoint = centerPosition, scale=scale)
      }
    }
  }
#endif
}

void ModelLoader::destroyModel() {}

void ModelLoader::loadModelGltf(uint8_t* buffer,
                                std::optional<float> scale,
                                std::optional<Position> centerPosition,
                                bool transformToUnitCube) {}

void ModelLoader::loadModelGltfAsync(
    uint8_t* buffer,
    std::optional<float> scale,
    std::optional<Position>,
    const std::function<uint8_t*(std::string)> callback,
    bool transformToUnitCube) {}

void ModelLoader::transformToUnitCube(std::optional<Position> centerPoint,
                                      std::optional<float> scale) {}
}  // namespace plugin_filament_view
