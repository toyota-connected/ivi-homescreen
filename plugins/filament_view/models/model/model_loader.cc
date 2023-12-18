
#include "model_loader.h"

#include <filesystem>
#include <fstream>
#include <sstream>

#include <filament/DebugRegistry.h>
#include <filament/LightManager.h>
#include <filament/RenderableManager.h>
#include <filament/TransformManager.h>
#include <math/mat4.h>
#include <math/vec3.h>
#include "asio/post.hpp"

#include "generated/resources/gltf_demo.h"
#include "materials/uberarchive.h"

#include "logging/logging.h"

namespace plugin_filament_view {

using ::filament::gltfio::AssetConfiguration;
using ::filament::gltfio::AssetLoader;
using ::filament::gltfio::ResourceConfiguration;
using ::filament::gltfio::ResourceLoader;

ModelLoader::ModelLoader(CustomModelViewer* modelViewer)
    : modelViewer_(modelViewer) {
  SPDLOG_TRACE("++ModelLoader::ModelLoader");
  assert(modelViewer_);
  auto engine = modelViewer->getEngine();
  materialProvider_ = ::filament::gltfio::createUbershaderProvider(
      engine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
  SPDLOG_DEBUG("UbershaderProvider MaterialsCount: {}",
               materialProvider_->getMaterialsCount());

  AssetConfiguration assetConfiguration{};
  assetConfiguration.engine = engine;
  assetConfiguration.materials = materialProvider_;
  assetLoader_ = AssetLoader::create(assetConfiguration);

  ResourceConfiguration resourceConfiguration{};
  resourceConfiguration.engine = engine;
  resourceConfiguration.normalizeSkinningWeights = true;
  resourceLoader_ =
      new ResourceLoader(resourceConfiguration);

  assetPath_ = modelViewer->getAssetPath();
  visibleScenes_.reset();
  SPDLOG_TRACE("--ModelLoader::ModelLoader");
}

ModelLoader::~ModelLoader() {
  delete resourceLoader_;
  resourceLoader_ = nullptr;

  if (assetLoader_) {
    AssetLoader::destroy(&assetLoader_);
  }
}

/**
 * Loads a monolithic binary glTF and populates the Filament scene.
 */
void ModelLoader::loadModelGlb(const std::vector<uint8_t>& buffer,
                               const Position* centerPosition,
                               float scale,
                               bool transform) {
  // TODO destroyModel();

  spdlog::debug("thread_id: 0x{:x}", pthread_self());
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

filament::math::mat4f inline fitIntoUnitCube(const ::filament::Aabb& bounds,
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

  auto engine = modelViewer_->getEngine();
  auto& tcm = engine->getTransformManager();
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

  auto engine = modelViewer_->getEngine();
  auto view = modelViewer_->getView();
  auto scene = modelViewer_->getScene();
  settings_ = modelViewer_->getSettings();

  auto& tm = engine->getTransformManager();
  auto& rm = engine->getRenderableManager();
  auto& lm = engine->getLightManager();

  auto renderableTreeItem = [this, &rm, scene](utils::Entity entity) {
    bool rvis = scene->hasEntity(entity);
    scene->addEntity(entity);

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

  auto lightTreeItem = [this, engine, scene](utils::Entity entity) {
    auto& lm = engine->getLightManager();
    bool lvis = scene->hasEntity(entity);
    // ImGui::Checkbox("visible", &lvis);
    lvis = true;

    if (lvis) {
      scene->addEntity(entity);
    } else {
      scene->remove(entity);
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

  ::filament::DebugRegistry& debug = engine->getDebugRegistry();

  // colorGradingUI(settings_, rangePlot_, curvePlot_, toneMapPlot_);

  // At this point, all View settings have been modified,
  //  so we can now push them into the Filament View.
  applySettings(engine, settings_.view, view);

  auto lights = utils::FixedCapacityVector<utils::Entity>::with_capacity(
      scene->getEntityCount());
  scene->forEach([&](utils::Entity entity) {
    if (lm.hasComponent(entity)) {
      lights.push_back(entity);
    }
  });

  applySettings(engine, settings_.lighting, indirectLight_, sunlight_,
                lights.data(), lights.size(), &lm, scene, view);

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
      scene->addEntity(asset_->getWireframe());
    } else {
      scene->remove(asset_->getWireframe());
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

std::future<std::string> ModelLoader::loadGlbFromAsset(
    const std::string& path,
    float scale,
    const Position* centerPosition,
    const bool isFallback) {
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());

  asio::post(*modelViewer_->getStrandContext(), [&, promise, path, scale,
                                                 centerPosition] {
    std::filesystem::path asset_path(assetPath_);
    asset_path /= path;
    if (path.empty() || !std::filesystem::exists(assetPath_)) {
      modelViewer_->setModelState(ModelState::error);
      promise->set_value("Asset path not valid");
    }
    SPDLOG_DEBUG("Attempting to load {}", asset_path.c_str());
    SPDLOG_DEBUG("scale: {}", scale);
    centerPosition->Print("centerPosition");

    modelViewer_->setModelState(ModelState::loading);

    std::ifstream stream(asset_path, std::ios::in | std::ios::binary);
    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(stream)),
                                std::istreambuf_iterator<char>());

    asset_ = assetLoader_->createAsset(buffer.data(), buffer.size());
    resourceLoader_->asyncBeginLoad(asset_);
    modelViewer_->setAnimator(asset_->getInstance()->getAnimator());
    asset_->releaseSourceData();
    instance_ = asset_->getInstance();
    auto settings = modelViewer_->getSettings();
    if (settings.viewer.autoScaleEnabled) {
      transformToUnitCube(centerPosition, scale);
    }
    visibleScenes_.set(1);

    // TODO error response @withContext Resource.Error(bufferResource.message ?:
    // "Couldn't load glb model from asset")

    modelViewer_->setModelState(isFallback ? ModelState::fallbackLoaded
                                           : ModelState::loaded);
    std::stringstream ss;
    ss << "Loaded glb model successfully from " << asset_path;
    promise->set_value(ss.str());
  });

  return future;
}

std::future<std::string> ModelLoader::loadGlbFromUrl(
    const std::string& url,
    float scale,
    const Position* centerPosition,
    bool isFallback) {
  spdlog::error("loadGlbFromUrl not implemented yet");
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());

  // TODO schedule from here
#if 0
      try {
        val buffer = NetworkClient.downloadFile(url) if (buffer != null) {
          modelViewer.modelLoader.loadModelGlb(buffer, true, centerPosition, scale)
          modelViewer.setModelState(
            if (isFallback) ModelState.FALLBACK_LOADED else ModelState.LOADED)
              return @withContext Resource.Success("Loaded glb model successfully from $url")
        }
        else {
          modelViewer.setModelState(ModelState.ERROR) return @withContext Resource.Error("Couldn't load glb model from url: $url")
        }
      } catch (e : Throwable) {
        modelViewer.setModelState(ModelState.ERROR) return @withContext Resource.Error("Couldn't load glb model from url: $url")
      }
    }
  }
#endif  // TODO

  if (url.empty()) {
    modelViewer_->setModelState(ModelState::error);
    promise->set_value("Url is empty");
  }

  modelViewer_->setModelState(ModelState::loading);
  // TODO post to platform runner
  modelViewer_->setModelState(isFallback ? ModelState::fallbackLoaded
                                         : ModelState::loaded);

  std::stringstream ss;
  ss << "Loaded glb model successfully from " << url;
  promise->set_value(ss.str());

  return future;
}

std::string ModelLoader::loadModel(Model* model) {
  std::string result;
  if (model->isGlb()) {
    if (!model->GetAssetPath().empty()) {
      auto f = loadGlbFromAsset(model->GetAssetPath(), model->GetScale(),
                                model->GetCenterPosition());
      f.wait();
      result = f.get();
    } else if (!model->GetUrl().empty()) {
      auto f = loadGlbFromUrl(model->GetUrl(), model->GetScale(),
                              model->GetCenterPosition());
      f.wait();
      result = f.get();
    }
  }
#if 0
    else {
    if (!model->GetAssetPath().empty()) {
      auto f = loadGltfFromAsset(model->GetAssetPath(), model->GetScale(),
                                 model->GetCenterPosition());
      f.wait();
      result = f.get();
    } else if (!model->GetUrl().empty()) {
      auto f = loadGltfFromUrl(model->GetUrl(), model->GetScale(),
                               model->GetCenterPosition());
      f.wait();
      result = f.get();
    }
  }
#endif
  return result;
}

}  // namespace plugin_filament_view
