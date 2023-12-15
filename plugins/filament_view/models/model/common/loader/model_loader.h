
#pragma once

#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/ResourceLoader.h>
#include "viewer/custom_model_viewer.h"

class CustomModelViewer;

namespace plugin_filament_view {

class ModelLoader {
 public:
  ModelLoader(CustomModelViewer* modelViewer,
              ::filament::gltfio::AssetLoader* assetLoader,
              ::filament::gltfio::ResourceLoader* resourceLoader);
  ~ModelLoader();

  /**
   * Frees all entities associated with the most recently-loaded model.
   */
  void destroyModel();

 private:
  // TODO std::optional<std::unique_ptr<filament::gltfio::FilamentAsset>>
  // asset_;
  ::filament::Engine* engine_;
  ::filament::gltfio::AssetLoader* assetLoader_;
  ::filament::gltfio::ResourceLoader* resourceLoader_;

  int readyRenderables_[128]{};  // add up to 128 entities at a time

  // private set

  // private var fetchResourcesJob: Job? = null

  /**
   * Loads a monolithic binary glTF and populates the Filament scene.
   */
  void loadModelGlb(uint8_t* buffer,
                    std::optional<Position> centerPosition,
                    std::optional<float> scale,
                    bool transformToUnitCube = false);

  /**
   * Loads a JSON-style glTF file and populates the Filament scene.
   * The given callback is triggered for each requested resource.
   */
  void loadModelGltf(uint8_t* buffer,
                     std::optional<float> scale,
                     std::optional<Position> centerPosition,
                     bool transformToUnitCube = false);

  /**
   * Loads a JSON-style glTF file and populates the Filament scene.
   *
   * The given callback is triggered from a worker thread for each requested
   * resource.
   */
  void loadModelGltfAsync(
      uint8_t* buffer,
      std::optional<float> scale,
      std::optional<Position>,
      const std::function<uint8_t*(std::string asset)> callback,
      bool transformToUnitCube = false);

  void fetchResources(
      ::filament::gltfio::FilamentAsset* asset,
      const std::function<uint8_t*(std::string asset)> callback);

  /**
   * Sets up a root transform on the current model to make it fit into a unit
   * cube.
   *
   * @param centerPoint Coordinate of center point of unit cube, defaults to <
   * 0, 0, -4 >
   */
  void transformToUnitCube(std::optional<Position> centerPoint,
                           std::optional<float> scale);

  // fun getModelTransform(): Mat4?

  /**
   * Removes the transformation that was set up via transformToUnitCube.
   */
  void clearRootTransform();

  void updateScene();

  void populateScene(::filament::gltfio::FilamentAsset* asset);

  // fun Int.getTransform(): Mat4

  // fun Int.setTransform(mat: Mat4)
};
}  // namespace plugin_filament_view
