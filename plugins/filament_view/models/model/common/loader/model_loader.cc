
#include "model_loader.h"

#include <filament/DebugRegistry.h>
#include <filament/LightManager.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <math/mat4.h>
#include <math/vec3.h>

#include "logging/logging.h"

namespace plugin_filament_view {

ModelLoader::ModelLoader(CustomModelViewer* modelViewer,
                         ::filament::Engine* engine,
                         ::filament::gltfio::AssetLoader* assetLoader,
                         ::filament::gltfio::ResourceLoader* resourceLoader,
                         asio::io_context::strand* strand)
    : engine_(engine),
      scene_(modelViewer->getScene()),
      view_(modelViewer->getView()),
      // sunlight_(modelViewer->getSunlight()),
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
  instance_ = asset_->getInstance();
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
  // TODO asset_->releaseSourceData();
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

filament::math::mat4f fitIntoUnitCube(const ::filament::Aabb& bounds,
                                      float zoffset) {
  filament::math::float3 minpt = bounds.min;
  filament::math::float3 maxpt = bounds.max;
  float maxExtent;
  maxExtent = std::max(maxpt.x - minpt.x, maxpt.y - minpt.y);
  maxExtent = std::max(maxExtent, maxpt.z - minpt.z);
  float scaleFactor = 2.0f / maxExtent;
  filament::math::float3 center = (minpt + maxpt) / 2.0f;
  center.z += zoffset / scaleFactor;
  return filament::math::mat4f::scaling(filament::math::float3(scaleFactor)) *
         filament::math::mat4f::translation(-center);
}

void ModelLoader::transformToUnitCube(const Position* centerPoint,
                                      float scale) {
  using namespace filament;
  using namespace filament::math;
  using namespace filament::gltfio;

  auto& tcm = engine_->getTransformManager();
  auto root = tcm.getInstance(asset_->getRoot());
  mat4f transform;
  if (settings_.viewer.autoScaleEnabled) {
    FilamentInstance* instance = asset_->getInstance();
    Aabb aabb =
        instance ? instance->getBoundingBox() : asset_->getBoundingBox();
    transform = fitIntoUnitCube(aabb, 4);
  }
  tcm.setTransform(root, transform);
}

void ModelLoader::populateScene(::filament::gltfio::FilamentAsset* asset) {
  if (isRemoteMode()) {
    return;
  }

  view_ = modelViewer_->getView();
  scene_ = modelViewer_->getScene();

  auto& tm = engine_->getTransformManager();
  auto& rm = engine_->getRenderableManager();
  auto& lm = engine_->getLightManager();

  auto renderableTreeItem = [this, &rm](utils::Entity entity) {
    bool rvis = scene_->hasEntity(entity);
    scene_->addEntity(entity);

    auto instance = rm.getInstance(entity);
    bool scaster = rm.isShadowCaster(instance);
    bool sreceiver = rm.isShadowReceiver(instance);
    // ImGui::Checkbox("casts shadows", &scaster);
    scaster = false;
    rm.setCastShadows(instance, scaster);
    // ImGui::Checkbox("receives shadows", &sreceiver);
    sreceiver = false;
    rm.setReceiveShadows(instance, sreceiver);

    rm.setScreenSpaceContactShadows(instance, true);
    auto numMorphTargets = rm.getMorphTargetCount(instance);
    if (numMorphTargets > 0) {
      bool selected = entity == currentMorphingEntity_;
      // ImGui::Checkbox("morphing", &selected);
      if (selected) {
        currentMorphingEntity_ = entity;
        morphWeights_.resize(numMorphTargets, 0.0f);
      } else {
        currentMorphingEntity_ = utils::Entity();
      }
    }
#if !defined(NDEBUG)
    size_t numPrims = rm.getPrimitiveCount(instance);
    for (size_t prim = 0; prim < numPrims; ++prim) {
      const char* material_name =
          rm.getMaterialInstanceAt(instance, prim)->getName();
      if (material_name) {
        SPDLOG_TRACE("prim {}: material {}", prim, material_name);
      } else {
        SPDLOG_TRACE("prim {}: (unnamed material)", prim);
      }
    }
#endif
  };

  auto lightTreeItem = [this](utils::Entity entity) {
    auto& lm = engine_->getLightManager();
    bool lvis = scene_->hasEntity(entity);
    // ImGui::Checkbox("visible", &lvis);
    lvis = true;

    if (lvis) {
      scene_->addEntity(entity);
    } else {
      scene_->remove(entity);
    }

    auto instance = lm.getInstance(entity);
    bool lcaster = lm.isShadowCaster(instance);
    // ImGui::Checkbox("shadow caster", &lcaster);
    lcaster = true;
    lm.setShadowCaster(instance, lcaster);
  };

  // Declare a std::function for tree nodes, it's an easy way to make a
  // recursive lambda.
  std::function<void(utils::Entity)> treeNode;
  treeNode = [&](utils::Entity entity) {
    auto tinstance = tm.getInstance(entity);
    auto rinstance = rm.getInstance(entity);
    auto linstance = lm.getInstance(entity);
    intptr_t treeNodeId = 1 + entity.getId();

    const char* name = asset_->getName(entity);
    auto getLabel = [&name, &rinstance, &linstance]() {
      if (name) {
        return name;
      }
      if (rinstance) {
        return "Mesh";
      }
      if (linstance) {
        return "Light";
      }
      return "Node";
    };
    const char* label = getLabel();
    SPDLOG_TRACE("treeNode: [{}] {}", treeNodeId, label);

    std::vector<utils::Entity> children(tm.getChildCount(tinstance));
    if (rinstance) {
      renderableTreeItem(entity);
    }
    if (linstance) {
      lightTreeItem(entity);
    }
    tm.getChildren(tinstance, children.data(), children.size());
    for (auto ce : children) {
      treeNode(ce);
    }
  };

  ::filament::DebugRegistry& debug = engine_->getDebugRegistry();

  // colorGradingUI(settings_, rangePlot_, curvePlot_, toneMapPlot_);

  // At this point, all View settings have been modified,
  //  so we can now push them into the Filament View.
  applySettings(engine_, settings_.view, view_);

  auto lights = utils::FixedCapacityVector<utils::Entity>::with_capacity(
      scene_->getEntityCount());
  scene_->forEach([&](utils::Entity entity) {
    if (lm.hasComponent(entity)) {
      lights.push_back(entity);
    }
  });

  applySettings(engine_, settings_.lighting, indirectLight_, sunlight_,
                lights.data(), lights.size(), &lm, scene_, view_);

  if (!isRemoteMode()) {
    // Show bounds
    // enableWireframe_ = true;
    treeNode(asset_->getRoot());

    if (instance_->getMaterialVariantCount() > 0) {
      int selectedVariant = currentVariant_;
      for (size_t i = 0, count = instance_->getMaterialVariantCount();
           i < count; ++i) {
        const char* label = instance_->getMaterialVariantName(i);
        // ImGui::RadioButton(label, &selectedVariant, i);
        //  selectedVariant =
      }
      if (selectedVariant != currentVariant_) {
        currentVariant_ = selectedVariant;
        instance_->applyMaterialVariant(currentVariant_);
      }
    }

    filament::gltfio::Animator& animator = *instance_->getAnimator();
    const size_t animationCount = animator.getAnimationCount();
    if (animationCount > 0) {
      int selectedAnimation = currentAnimation_;
      // Disable
      //  ImGui::RadioButton("Disable", &selectedAnimation, -1);
      selectedAnimation = -1;
      // Apply all animations
      selectedAnimation = animationCount;
      // Cross fade
      // ImGui::SliderFloat("Cross fade", &mCrossFadeDuration, 0.0f, 2.0f,
      //                    "%4.2f seconds", ImGuiSliderFlags_AlwaysClamp);
      for (size_t i = 0; i < animationCount; ++i) {
        std::string label = animator.getAnimationName(i);
        if (label.empty()) {
          label = "Unnamed " + std::to_string(i);
        }
        SPDLOG_TRACE("animation: [{}] {}", i, label.c_str());
        // ImGui::RadioButton(label.c_str(), &selectedAnimation, i);
      }
      if (selectedAnimation != currentAnimation_) {
        previousAnimation_ = currentAnimation_;
        currentAnimation_ = selectedAnimation;
        resetAnimation_ = true;
      }
      // Show rest pose
      showingRestPose_ = true;
    }

    // Morphing
    if (currentMorphingEntity_) {
      const bool isAnimating =
          currentAnimation_ > 0 && animator.getAnimationCount() > 0;
      if (isAnimating) {
        // ImGui::BeginDisabled();
      }
      for (int i = 0; i != morphWeights_.size(); ++i) {
        const char* name =
            asset_->getMorphTargetNameAt(currentMorphingEntity_, i);
        std::string label = name ? name : "Unnamed target " + std::to_string(i);
        SPDLOG_DEBUG("label: {}", label);
        // Morph Weights
        morphWeights_[i] = 0.0f;  // 0.0f, 1.0
      }
      if (isAnimating) {
        // ImGui::EndDisabled();
      }
      if (!isAnimating) {
        auto instance = rm.getInstance(currentMorphingEntity_);
        rm.setMorphWeights(instance, morphWeights_.data(),
                           morphWeights_.size());
      }
    }

    if (enableWireframe_) {
      scene_->addEntity(asset_->getWireframe());
    } else {
      scene_->remove(asset_->getWireframe());
    }
  }
}

void ModelLoader::updateScene() {
  // Allow the resource loader to finalize textures that have become ready.
  resourceLoader_->asyncUpdateLoad();

  // Add render-able entities to the scene as they become ready.
  populateScene(asset_);
}

void ModelLoader::removeAsset() {
  if (!isRemoteMode()) {
    modelViewer_->getScene()->removeEntities(asset_->getEntities(),
                                             asset_->getEntityCount());
    asset_ = nullptr;
  }
}

}  // namespace plugin_filament_view
