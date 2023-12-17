
#pragma once

#include <asio/io_context_strand.hpp>
#include <filament/IndirectLight.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/ResourceLoader.h>

#include "viewer/custom_model_viewer.h"

#include "settings.h"

class CustomModelViewer;

namespace plugin_filament_view {

class ModelLoader {
 public:
  ModelLoader(CustomModelViewer* modelViewer,
              ::filament::Engine* engine,
              ::filament::gltfio::AssetLoader* assetLoader,
              ::filament::gltfio::ResourceLoader* resourceLoader,
              asio::io_context::strand* strand);
  /**
   * Frees all entities associated with the most recently-loaded model.
   */
  ~ModelLoader();

  /**
   * Loads a monolithic binary glTF and populates the Filament scene.
   */
  void loadModelGlb(const std::vector<uint8_t>& buffer,
                    const Position* centerPosition,
                    float scale,
                    bool transform = false);

  /**
   * Loads a JSON-style glTF file and populates the Filament scene.
   * The given callback is triggered for each requested resource.
   */
  void loadModelGltf(const std::vector<uint8_t>& buffer,
                     const Position* centerPosition,
                     float scale,
                     bool transform);

  void updateScene();

  /**
   * Enables a built-in light source (useful for creating shadows).
   * Defaults to true.
   */
  void enableSunlight(bool b) { settings_.lighting.enableSunlight = b; }

  /**
   * Enables dithering on the view.
   * Defaults to true.
   */
  void enableDithering(bool b) {
    settings_.view.dithering =
        b ? filament::Dithering::TEMPORAL : filament::Dithering::NONE;
  }

  /**
   * Enables FXAA antialiasing in the post-process pipeline.
   * Defaults to true.
   */
  void enableFxaa(bool b) {
    settings_.view.antiAliasing =
        b ? filament::AntiAliasing::FXAA : filament::AntiAliasing::NONE;
  }

  /**
   * Enables hardware-based MSAA antialiasing.
   * Defaults to true.
   */
  void enableMsaa(bool b) {
    settings_.view.msaa.sampleCount = 4;
    settings_.view.msaa.enabled = b;
  }

  /**
   * Enables screen-space ambient occlusion in the post-process pipeline.
   * Defaults to true.
   */
  void enableSSAO(bool b) { settings_.view.ssao.enabled = b; }

  /**
   * Enables Bloom.
   * Defaults to true.
   */
  void enableBloom(bool bloom) { settings_.view.bloom.enabled = bloom; }

  /**
   * Adjusts the intensity of the IBL.
   * See also IndirectLight::setIntensity().
   * Defaults to 30000.0.
   */
  void setIBLIntensity(float brightness) {
    settings_.lighting.iblIntensity = brightness;
  }

 private:
  // Immutable properties set from the constructor.
  ::filament::Engine* const engine_;
  ::filament::Scene* scene_;
  ::filament::View* view_;
  utils::Entity sunlight_;
  CustomModelViewer* modelViewer_;
  ::filament::gltfio::AssetLoader* assetLoader_;
  ::filament::gltfio::ResourceLoader* resourceLoader_;
  asio::io_context::strand* strand_;

  // Properties that can be changed from the application.
  filament::gltfio::FilamentAsset* asset_ = nullptr;
  filament::gltfio::FilamentInstance* instance_ = nullptr;
  filament::IndirectLight* indirectLight_ = nullptr;

  // Parameters that can be changed
  int currentAnimation_ = 0;  // -1 means not playing animation and count means
                              // plays all of them (0-based index)
  int currentVariant_ = 0;
  bool enableWireframe_ = false;
  int vsmMsaaSamplesLog2_ = 1;
  ::filament::viewer::Settings settings_;
  int mSidebarWidth_;
  uint32_t flags_;
  utils::Entity currentMorphingEntity_;
  std::vector<float> morphWeights_;
  ::filament::gltfio::NodeManager::SceneMask visibleScenes_;
  bool showingRestPose_ = false;

  // Cross fade animation parameters.
  float crossFadeDuration_ =
      0.5f;  // number of seconds to transition between animations
  int previousAnimation_ = -1;  // zero-based index of the previous animation
  double currentStartTime_ =
      0.0f;  // start time of most recent cross-fade (seconds)
  double previousStartTime_ =
      0.0f;  // start time of previous cross-fade (seconds)
  bool resetAnimation_ =
      true;  // set when building ImGui widgets, honored in applyAnimation

  // Color grading UI state.
  float toneMapPlot_[1024];
  float rangePlot_[1024 * 3];
  float curvePlot_[1024 * 3];

  // private var fetchResourcesJob: Job? = null

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
  void transformToUnitCube(const Position* centerPoint, float scale);

  // fun getModelTransform(): Mat4?

  /**
   * Removes the transformation that was set up via transformToUnitCube.
   */
  void clearRootTransform();

  void populateScene(::filament::gltfio::FilamentAsset* asset);

  bool isRemoteMode() const { return asset_ == nullptr; }

  void removeAsset();

  // fun Int.getTransform(): Mat4

  // fun Int.setTransform(mat: Mat4)
};
}  // namespace plugin_filament_view
