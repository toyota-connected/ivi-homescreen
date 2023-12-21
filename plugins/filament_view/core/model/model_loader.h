
#pragma once

#include <filament/IndirectLight.h>
#include <filament/TransformManager.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/ResourceLoader.h>
#include "asio/io_context_strand.hpp"

#include "model.h"
#include "viewer/custom_model_viewer.h"
#include "viewer/settings.h"

namespace plugin_filament_view {

class CustomModelViewer;

class Model;

class ModelLoader {
 public:
  ModelLoader(CustomModelViewer* modelViewer);

  /**
   * Frees all entities associated with the most recently-loaded model.
   */
  ~ModelLoader();

  void destroyModel();

  /**
   * Loads a monolithic binary glTF and populates the Filament scene.
   */
  void loadModelGlb(const std::vector<uint8_t>& buffer,
                    const Position* centerPosition,
                    float scale,
                    bool transformToUnitCube = false);

  /**
   * Loads a JSON-style glTF file and populates the Filament scene.
   * The given callback is triggered for each requested resource.
   */
  void loadModelGltf(const std::vector<uint8_t>& buffer,
                     const Position* centerPosition,
                     float scale,
                     std::function<const ::filament::backend::BufferDescriptor&(
                         std::string uri)>& callback,
                     bool transform = false);

  ::filament::gltfio::FilamentAsset* getAsset() const { return asset_; };

  const ::filament::math::mat4f& getModelTransform();

  void updateScene();

 private:
  ::filament::Engine* engine_;
  std::string assetPath_;
  utils::Entity sunlight_;
  CustomModelViewer* modelViewer_;
  ::filament::gltfio::AssetLoader* assetLoader_;
  ::filament::gltfio::MaterialProvider* materialProvider_;
  ::filament::gltfio::ResourceLoader* resourceLoader_;

  // Properties that can be changed from the application.
  filament::gltfio::FilamentAsset* asset_ = nullptr;
  filament::gltfio::FilamentInstance* instance_ = nullptr;
  filament::IndirectLight* indirectLight_ = nullptr;

  utils::Entity readyRenderables_[128];

  ::filament::viewer::Settings settings_;
  std::vector<float> morphWeights_;
  ::filament::gltfio::NodeManager::SceneMask visibleScenes_;

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
  void transformToUnitCube(const Position* centerPoint, float scale);

  /**
   * Removes the transformation that was set up via transformToUnitCube.
   */
  void clearRootTransform();

  void populateScene(::filament::gltfio::FilamentAsset* asset);

  bool isRemoteMode() const { return asset_ == nullptr; }

  void removeAsset();

  ::filament::mat4f getTransform();

  void setTransform(::filament::mat4f mat);
};
}  // namespace plugin_filament_view
