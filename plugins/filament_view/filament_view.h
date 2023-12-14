/*
 * Copyright 2023 Toyota Connected North America
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

#pragma once

#include <shell/platform/embedder/embedder.h>

#include <memory>
#include <optional>
#include <vector>

#include <filament/Engine.h>
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
#include "viewer/custom_model_viewer.h"
#include "view/flutter_view.h"

extern "C" {
extern const uint8_t UBERARCHIVE_PACKAGE[];
extern int UBERARCHIVE_DEFAULT_OFFSET;
extern size_t UBERARCHIVE_DEFAULT_SIZE;
}
#define UBERARCHIVE_DEFAULT_DATA \
  (UBERARCHIVE_PACKAGE + UBERARCHIVE_DEFAULT_OFFSET)

class FlutterView;

namespace plugin_filament_view {

using namespace plugin_filament_view::material::texture;
using namespace plugin_filament_view::material::texture;

class FilamentView final : public PlatformView {

 private:

  void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  static void OnPlatformChannel(const FlutterPlatformMessage* message,
                                void* userdata);
  static void OnModelState(const FlutterPlatformMessage* message,
                           void* userdata);
  static void OnSceneState(const FlutterPlatformMessage* message,
                           void* userdata);
  static void OnShapeState(const FlutterPlatformMessage* message,
                           void* userdata);
  static void OnRenderer(const FlutterPlatformMessage* message, void* userdata);
};

}  // namespace plugin_filament_view