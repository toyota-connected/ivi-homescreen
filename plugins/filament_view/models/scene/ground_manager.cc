#include "ground_manager.h"

#include <asio/post.hpp>
#include <filament/Engine.h>
#include <filament/RenderableManager.h>
#include <filament/Scene.h>
#include <filament/TransformManager.h>
#include <filament/View.h>
#include <math/mat3.h>
#include <math/norm.h>
#include <math/vec3.h>
#include <math/vec4.h>

#include "logging/logging.h"

#include "generated/resources/gltf_demo.h"
#include "materials/uberarchive.h"

namespace plugin_filament_view {

using ::filament::Aabb;
using ::filament::RenderableManager;
using ::filament::VertexAttribute;
using ::filament::math::float3;
using ::filament::math::mat3f;
using ::filament::math::mat4f;
using ::filament::math::packSnorm16;
using ::filament::math::short4;

GroundManager::GroundManager(CustomModelViewer* model_viewer,
                             const std::string& flutter_assets_path)
    : model_viewer_(model_viewer), flutterAssetsPath_(flutter_assets_path) {
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

std::future<void> GroundManager::createGround() {
  SPDLOG_DEBUG("++GroundManager::createGround");
  const auto promise(std::make_shared<std::promise<void>>());
  auto future(promise->get_future());
  asio::post(*model_viewer_->getStrandContext(),[&, promise]{
    auto engine = model_viewer_->getEngine();

    auto& em = engine->getEntityManager();
    ::filament::Material* shadowMaterial =
        ::filament::Material::Builder()
            .package(GLTF_DEMO_GROUNDSHADOW_DATA,
                     static_cast<size_t>(GLTF_DEMO_GROUNDSHADOW_SIZE))
            .build(*engine);
    auto& viewerOptions = model_viewer_->getSettings().viewer;
    shadowMaterial->setDefaultParameter("strength",
                                        viewerOptions.groundShadowStrength);

    const static uint32_t indices[] = {0, 1, 2, 2, 3, 0};

    auto asset = model_viewer_->getAsset();
    Aabb aabb = asset->getBoundingBox();
    if (!model_viewer_->getActualSize()) {
      mat4f const transform = fitIntoUnitCube(aabb, 4);
      aabb = aabb.transform(transform);
    }

    float3 planeExtent{10.0f * aabb.extent().x, 0.0f, 10.0f * aabb.extent().z};

    const static float3 vertices[] = {
        {-planeExtent.x, 0, -planeExtent.z},
        {-planeExtent.x, 0, planeExtent.z},
        {planeExtent.x, 0, planeExtent.z},
        {planeExtent.x, 0, -planeExtent.z},
    };

    short4 const tbn =
        packSnorm16(mat3f::packTangentFrame(mat3f{float3{1.0f, 0.0f, 0.0f},
                                                  float3{0.0f, 0.0f, 1.0f},
                                                  float3{0.0f, 1.0f, 0.0f}})
                        .xyzw);

    const static ::filament::math::short4 normals[]{tbn, tbn, tbn, tbn};

    VertexBuffer* vertexBuffer =
        VertexBuffer::Builder()
            .vertexCount(4)
            .bufferCount(2)
            .attribute(VertexAttribute::POSITION, 0,
                       VertexBuffer::AttributeType::FLOAT3)
            .attribute(VertexAttribute::TANGENTS, 1,
                       VertexBuffer::AttributeType::SHORT4)
            .normalized(VertexAttribute::TANGENTS)
            .build(*engine);

    vertexBuffer->setBufferAt(
        *engine, 0,
        VertexBuffer::BufferDescriptor(
            vertices, vertexBuffer->getVertexCount() * sizeof(vertices[0])));
    vertexBuffer->setBufferAt(
        *engine, 1,
        VertexBuffer::BufferDescriptor(
            normals, vertexBuffer->getVertexCount() * sizeof(normals[0])));

    IndexBuffer* indexBuffer =
        IndexBuffer::Builder().indexCount(6).build(*engine);

    indexBuffer->setBuffer(
        *engine, IndexBuffer::BufferDescriptor(
                     indices, indexBuffer->getIndexCount() * sizeof(uint32_t)));

    Entity groundPlane = em.create();
    RenderableManager::Builder(1)
        .boundingBox({{}, {planeExtent.x, 1e-4f, planeExtent.z}})
        .material(0, shadowMaterial->getDefaultInstance())
        .geometry(0, RenderableManager::PrimitiveType::TRIANGLES, vertexBuffer,
                  indexBuffer, 0, 6)
        .culling(false)
        .receiveShadows(true)
        .castShadows(false)
        .build(*engine, groundPlane);

    auto scene = model_viewer_->getScene();
    scene->addEntity(groundPlane);

    auto& tcm = engine->getTransformManager();
    tcm.setTransform(tcm.getInstance(groundPlane),
                     mat4f::translation(float3{0, aabb.min.y, -4}));

    auto& rcm = engine->getRenderableManager();
    auto instance = rcm.getInstance(groundPlane);
    rcm.setLayerMask(instance, 0xff, 0x00);

    groundPlane_ = groundPlane;
    groundVertexBuffer_ = vertexBuffer;
    groundIndexBuffer_ = indexBuffer;
    groundMaterial_ = shadowMaterial;
    promise->set_value();
  });
  SPDLOG_DEBUG("--GroundManager::createGround");
  return future;
}
}  // namespace plugin_filament_view