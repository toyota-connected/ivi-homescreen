
#include "camera_manager.h"

#include "logging/logging.h"

namespace plugin_filament_view {
CameraManager::CameraManager(CustomModelViewer* model_viewer)
    : model_viewer_(model_viewer),
      engine_(model_viewer->getEngine()),
      view_(model_viewer->getView()) {
  SPDLOG_TRACE("++CameraManager::CameraManager");
  utils::EntityManager& em = utils::EntityManager::get();
  cameraEntity_ = em.create();
  camera_ = engine_->createCamera(cameraEntity_);
  SPDLOG_TRACE("--CameraManager::CameraManager");
}

void CameraManager::updateCamera(Camera* camera) {
  SPDLOG_DEBUG("CameraManager::updateCamera");
}

void CameraManager::lookAtDefaultPosition() {
  // SPDLOG_DEBUG("CameraManager::lookAtDefaultPosition");
  view_->setCamera(camera_);
  const auto size = view_->getViewport();
  displaySize_ =
      ImVec2(static_cast<float>(size.width), static_cast<float>(size.height));
  displayFramebufferScale_.x = 1;
  displayFramebufferScale_.y = 1;
  camera_->setProjection(::filament::Camera::Projection::ORTHO, 0.0,
                         double(size.width), double(size.height), 0.0, 0.0,
                         1.0);
  camera_->lookAt({0.0f, 0.0f, kCameraDist}, kCameraCenter, kCameraUp);
}

void CameraManager::destroyCamera() {
  SPDLOG_DEBUG("++CameraManager::destroyCamera");
  engine_->destroyCameraComponent(cameraEntity_);
  utils::EntityManager::get().destroy(cameraEntity_);
  SPDLOG_DEBUG("--CameraManager::destroyCamera");
}

}  // namespace plugin_filament_view