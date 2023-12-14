
#ifndef FLUTTER_PLUGIN_FILAMENT_VIEW_PLUGIN_H_
#define FLUTTER_PLUGIN_FILAMENT_VIEW_PLUGIN_H_

#include <flutter/event_channel.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>

#include <wayland-client.h>

#include <filament/Engine.h>
#include <filament/Viewport.h>
#include <filament/View.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/ResourceLoader.h>

#include "models/model/animation_manager.h"
#include "models/model/glb/loader/glb_loader.h"
#include "models/model/gltf/loader/gltf_loader.h"
#include "models/model/model.h"
#include "models/scene/camera/camera_manager.h"
#include "models/scene/ground_manager.h"
#include "models/scene/indirect_light/indirect_light_manager.h"
#include "models/scene/light/light_manager.h"
#include "models/scene/material/material_manager.h"
#include "models/scene/scene.h"
#include "models/scene/skybox/skybox_manager.h"
#include "models/shapes/shape.h"
#include "models/shapes/shape_manager.h"
#include "platform_views/platform_view.h"

#include <memory>

#include "flutter_desktop_engine_state.h"
#include "flutter_homescreen.h"
#include "view/flutter_view.h"
#include "wayland/display.h"

#include "messages.g.h"

namespace plugin_filament_view {

class FilamentViewPlugin : public flutter::Plugin,
                           public FilamentViewApi,
                           public ModelStateChannelApi,
                           public SceneStateApi,
                           public ShapeStateApi,
                           public RendererChannelApi,
                           public PlatformView {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar,
                                    int32_t id,
                                    std::string viewType,
                                    int32_t direction,
                                    double width,
                                    double height,
                                    const std::vector<uint8_t>& params,
                                    std::string assetDirectory,
                                    FlutterDesktopEngineRef engine);

  FilamentViewPlugin(int32_t id,
                     std::string viewType,
                     int32_t direction,
                     double width,
                     double height,
                     const std::vector<uint8_t>& params,
                     std::string assetDirectory,
                     FlutterDesktopEngineState* state);

  ~FilamentViewPlugin() override;

  void Resize(double width, double height) override;

  void SetDirection(int32_t direction) override;

  void SetOffset(double left, double top) override;

  void Dispose(bool hybrid) override;

  void ChangeAnimationByIndex(
      const int32_t index,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeAnimationByName(
      std::string name,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void GetAnimationNames(
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void GetAnimationCount(
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void GetCurrentAnimationIndex(
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void GetAnimationNameByIndex(
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeSkyboxByAsset(
      std::string path,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeSkyboxByUrl(
      std::string url,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeSkyboxByHdrAsset(
      std::string path,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeSkyboxByHdrUrl(
      std::string url,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeSkyboxColor(
      std::string color,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeToTransparentSkybox(
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeLightByKtxAsset(
      std::string path,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeLightByKtxUrl(
      std::string url,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeLightByIndirectLight(
      std::string path,
      double intensity,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeLightByHdrUrl(
      std::string path,
      double intensity,
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;
  void ChangeToDefaultIndirectLight(
      const std::function<void(std::optional<FlutterError> reply)> result)
      override;

  // Disallow copy and assign.
  FilamentViewPlugin(const FilamentViewPlugin&) = delete;
  FilamentViewPlugin& operator=(const FilamentViewPlugin&) = delete;

 private:
  struct _native_window {
    struct wl_display* display;
    struct wl_surface* surface;
    uint32_t width;
    uint32_t height;
  } native_window_;

  FlutterView* view_;
  wl_display* display_{};
  wl_surface* surface_;
  wl_surface* parent_surface_;
  wl_callback* callback_;
  wl_subsurface* subsurface_{};

  static void OnFrame(void* data, wl_callback* callback, uint32_t time);
  static const wl_callback_listener frame_listener;

  const std::string flutterAssetsPath_;

  std::optional<int32_t> currentAnimationIndex_;

  std::optional<std::unique_ptr<plugin_filament_view::Model>> model_;
  std::optional<std::unique_ptr<plugin_filament_view::Scene>> scene_;
  std::optional<std::unique_ptr<
      std::vector<std::unique_ptr<plugin_filament_view::Shape>>>>
      shapes_;

  ::filament::Engine* engine_;
  ::filament::SwapChain* swapChain_;
  ::filament::Renderer* renderer_;
  ::filament::gltfio::MaterialProvider* materialProvider_;
  ::filament::gltfio::AssetLoader* assetLoader_;
  std::unique_ptr<filament::gltfio::ResourceLoader> resourceLoader_;

  std::unique_ptr<CustomModelViewer> modelViewer_;

  // std::unique_ptr<IBLProfiler> iblProfiler_;

  std::unique_ptr<plugin_filament_view::models::glb::GlbLoader> glbLoader_;
  std::unique_ptr<plugin_filament_view::models::gltf::GltfLoader> gltfLoader_;
  std::unique_ptr<LightManager> lightManager_;
  std::unique_ptr<IndirectLightManager> indirectLightManager_;
  std::unique_ptr<SkyboxManager> skyboxManager_;
  std::unique_ptr<AnimationManager> animationManager_;
  CameraManager* cameraManager_{};
  std::unique_ptr<GroundManager> groundManager_;
  std::unique_ptr<MaterialManager> materialManager_;
  std::unique_ptr<ShapeManager> shapeManager_;

  void setUpViewer();
  void setUpGround();
  void setUpCamera();
  void setUpSkybox();
  void setUpLight();
  void setUpIndirectLight();
  void setUpLoadingModel();
  void setUpShapes();

  void DrawFrame(uint32_t time) const;
};

}  // namespace plugin_filament_view

#endif  // FLUTTER_PLUGIN_FILAMENT_VIEW_PLUGIN_H_
