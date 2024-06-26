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

#include "ground_manager.h"

#include <filament/Engine.h>
#include <filament/RenderableManager.h>
#include <filament/View.h>
#include <math/mat3.h>
#include <math/norm.h>
#include <math/vec3.h>
#include "asio/post.hpp"

#include "plugins/common/common.h"

namespace plugin_filament_view {

using ::filament::Aabb;
using ::filament::IndexBuffer;
using ::filament::RenderableManager;
using ::filament::VertexAttribute;
using ::filament::VertexBuffer;
using ::filament::math::float3;
using ::filament::math::mat3f;
using ::filament::math::mat4f;
using ::filament::math::packSnorm16;
using ::filament::math::short4;
using ::utils::Entity;

GroundManager::GroundManager(CustomModelViewer* modelViewer,
                             MaterialManager* material_manager,
                             Ground* ground)
    : modelViewer_(modelViewer),
      materialManager_(material_manager),
      engine_(modelViewer->getFilamentEngine()),
      ground_(ground),
      plane_geometry_(nullptr) {
  SPDLOG_TRACE("++GroundManager::GroundManager");
  SPDLOG_TRACE("--GroundManager::GroundManager");
}

mat4f inline fitIntoUnitCube(const Aabb& bounds, float zoffset) {
  float3 minpt = bounds.min;
  float3 maxpt = bounds.max;
  float maxExtent;
  maxExtent = std::max(maxpt.x - minpt.x, maxpt.y - minpt.y);
  maxExtent = std::max(maxExtent, maxpt.z - minpt.z);
  float scaleFactor = 2.0f / maxExtent;
  float3 center = (minpt + maxpt) / 2.0f;
  center.z += zoffset / scaleFactor;
  return mat4f::scaling(float3(scaleFactor)) * mat4f::translation(-center);
}

std::future<Resource<std::string_view>> GroundManager::createGround() const {
  SPDLOG_TRACE("++GroundManager::createGround");
  try {
    const auto promise =
        std::make_shared<std::promise<Resource<std::string_view>>>();
    auto future = promise->get_future();

    modelViewer_->setGroundState(SceneState::LOADING);

    if (!ground_) {
      spdlog::error("[flinament_view] Ground is not provided");
      modelViewer_->setGroundState(SceneState::ERROR);
      promise->set_value(
          Resource<std::string_view>::Error("Ground must be provided"));
      SPDLOG_TRACE("--GroundManager::createGround");
      return future;
    }

    if (!ground_->size_) {
      spdlog::error("[flinament_view] Ground size is not provided");
      modelViewer_->setGroundState(SceneState::ERROR);
      promise->set_value(
          Resource<std::string_view>::Error("Size must be provided"));
      SPDLOG_TRACE("--GroundManager::createGround");
      return future;
    }

    post(modelViewer_->getStrandContext(), [&, promise] {
      try {
        auto materialInstanceResult =
            materialManager_->getMaterialInstance(ground_->material_.get());
        auto modelTransform = modelViewer_->getModelTransform();
        float3 center;

        if (ground_->isBelowModel_ && modelTransform.has_value()) {
          center.x = modelTransform.value()[0][3];
          center.y = modelTransform.value()[1][3];
          center.z = modelTransform.value()[2][3];
        } else {
          if (ground_->center_position_ == nullptr) {
            spdlog::warn(
                "[filament_view] Center position is not provided, setting to "
                "default (0,0,0)");
            ground_->center_position_ =
                std::make_unique<::filament::math::float3>(0.0f, 0.0f, 0.0f);
          }
          center.x = ground_->center_position_->x;
          center.y = ground_->center_position_->y;
          center.z = ground_->center_position_->z;
        }

        // Rest of the function logic...

        // Example of another potential failure point wrapped in try-catch
        try {
          auto& em = utils::EntityManager::get();
          const static uint32_t indices[] = {0, 1, 2, 2, 3, 0};

          Aabb aabb =
              modelViewer_->getModelLoader()->getAsset()->getBoundingBox();
          mat4f const transform = fitIntoUnitCube(aabb, 4);
          aabb = aabb.transform(transform);

          float3 planeExtent{10.0f * aabb.extent().x, 0.0f,
                             10.0f * aabb.extent().z};

          const static float3 vertices[] = {
              {-planeExtent.x, 0, -planeExtent.z},
              {-planeExtent.x, 0, planeExtent.z},
              {planeExtent.x, 0, planeExtent.z},
              {planeExtent.x, 0, -planeExtent.z},
          };

          short4 const tbn = packSnorm16(
              mat3f::packTangentFrame(mat3f{float3{1.0f, 0.0f, 0.0f},
                                            float3{0.0f, 0.0f, 1.0f},
                                            float3{0.0f, 1.0f, 0.0f}})
                  .xyzw);

          const static short4 normals[]{tbn, tbn, tbn, tbn};

          VertexBuffer* vertexBuffer =
              VertexBuffer::Builder()
                  .vertexCount(4)
                  .bufferCount(2)
                  .attribute(VertexAttribute::POSITION, 0,
                             VertexBuffer::AttributeType::FLOAT3)
                  .attribute(VertexAttribute::TANGENTS, 1,
                             VertexBuffer::AttributeType::SHORT4)
                  .normalized(VertexAttribute::TANGENTS)
                  .build(*engine_);

          vertexBuffer->setBufferAt(
              *engine_, 0,
              VertexBuffer::BufferDescriptor(
                  vertices,
                  vertexBuffer->getVertexCount() * sizeof(vertices[0])));
          vertexBuffer->setBufferAt(
              *engine_, 1,
              VertexBuffer::BufferDescriptor(
                  normals,
                  vertexBuffer->getVertexCount() * sizeof(normals[0])));

          IndexBuffer* indexBuffer =
              IndexBuffer::Builder().indexCount(6).build(*engine_);

          indexBuffer->setBuffer(
              *engine_,
              IndexBuffer::BufferDescriptor(
                  indices, indexBuffer->getIndexCount() * sizeof(uint32_t)));

          Entity const groundPlane = em.create();
          RenderableManager::Builder(1)
              .boundingBox({{}, {planeExtent.x, 1e-4f, planeExtent.z}})
              .material(0, materialInstanceResult.getData().value())
              .geometry(0, RenderableManager::PrimitiveType::TRIANGLES,
                        vertexBuffer, indexBuffer, 0, 6)
              .culling(false)
              .receiveShadows(true)
              .castShadows(false)
              .build(*engine_, groundPlane);

          modelViewer_->getFilamentScene()->addEntity(groundPlane);

          auto& tcm = engine_->getTransformManager();
          tcm.setTransform(tcm.getInstance(groundPlane),
                           mat4f::translation(float3{0, aabb.min.y, -4}));

          auto& rcm = engine_->getRenderableManager();
          auto instance = rcm.getInstance(groundPlane);
          rcm.setLayerMask(instance, 0xff, 0x00);

          modelViewer_->setGroundState(SceneState::LOADED);
          promise->set_value(Resource<std::string_view>::Success(
              "Ground created successfully"));
        } catch (const std::exception& e) {
          spdlog::error(
              "[filament_view] Exception caught during ground creation: {}",
              e.what());
          modelViewer_->setGroundState(SceneState::ERROR);
          promise->set_value(Resource<std::string_view>::Error(e.what()));
        } catch (...) {
          spdlog::error(
              "[filament_view] Unknown exception caught during ground "
              "creation");
          modelViewer_->setGroundState(SceneState::ERROR);
          promise->set_value(Resource<std::string_view>::Error(
              "Unknown error during ground creation"));
        }
      } catch (const std::exception& e) {
        spdlog::error("[filament_view] Exception caught: {}", e.what());
        modelViewer_->setGroundState(SceneState::ERROR);
        promise->set_value(Resource<std::string_view>::Error(e.what()));
      } catch (...) {
        spdlog::error("[filament_view] Unknown exception caught");
        modelViewer_->setGroundState(SceneState::ERROR);
        promise->set_value(Resource<std::string_view>::Error("Unknown error"));
      }
    });

    SPDLOG_TRACE("--GroundManager::createGround");
    return future;
  } catch (const std::exception& e) {
    spdlog::error("[filament_view] Exception caught: {}", e.what());
    modelViewer_->setGroundState(SceneState::ERROR);
    std::promise<Resource<std::string_view>> promise;
    promise.set_value(Resource<std::string_view>::Error(e.what()));
    return promise.get_future();
  } catch (...) {
    spdlog::error("[filament_view] Unknown exception caught");
    modelViewer_->setGroundState(SceneState::ERROR);
    std::promise<Resource<std::string_view>> promise;
    promise.set_value(Resource<std::string_view>::Error("Unknown error"));
    return promise.get_future();
  }
}

std::future<Resource<std::string_view>> GroundManager::updateGround(
    Ground* /* newGround */) {
  return {};
}

std::future<Resource<std::string_view>> GroundManager::updateGroundMaterial(
    Material* /* newMaterial */) {
  return {};
}

void GroundManager::Print(const char* tag) {}

}  // namespace plugin_filament_view