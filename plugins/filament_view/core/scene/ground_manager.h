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

#pragma once

#include "ground.h"

#include <filament/IndexBuffer.h>

#include "core/include/resource.h"
#include "core/scene/material/model/material.h"
#include "viewer/custom_model_viewer.h"

namespace plugin_filament_view {

class Ground;

class Material;

class MaterialManager;

class CustomModelViewer;

class GroundManager {
 public:
  GroundManager(CustomModelViewer* modelViewer,
                MaterialManager* material_manager,
                Ground* ground);

  [[nodiscard]] std::future<Resource<std::string_view>> createGround() const;

  static std::future<Resource<std::string_view>> updateGround(
      Ground* newGround);

  static std::future<Resource<std::string_view>> updateGroundMaterial(
      Material* newMaterial);

  void Print(const char* tag);

  // Disallow copy and assign.
  GroundManager(const GroundManager&) = delete;
  GroundManager& operator=(const GroundManager&) = delete;

 private:
  CustomModelViewer* modelViewer_;
  MaterialManager* materialManager_;

  ::filament::Engine* engine_;
  Ground* ground_;
  void* plane_geometry_;
  // TODO PlaneGeometry* plane_geometry_;

  ::utils::Entity groundPlane_;
  ::filament::VertexBuffer* groundVertexBuffer_{};
  ::filament::IndexBuffer* groundIndexBuffer_{};
  ::filament::Material* groundMaterial_{};
};
}  // namespace plugin_filament_view