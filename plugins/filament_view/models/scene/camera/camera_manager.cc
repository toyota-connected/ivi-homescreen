
#include "camera_manager.h"

#include "logging/logging.h"

namespace plugin_filament_view {
CameraManager::CameraManager(CustomModelViewer* model_viewer)
    : model_viewer_(model_viewer),
      engine_(model_viewer->getEngine()),
      view_(model_viewer->getView()) {
  SPDLOG_TRACE("++CameraManager::CameraManager");
  setDefaultCamera();
  SPDLOG_TRACE("--CameraManager::CameraManager");
}

void CameraManager::setDefaultCamera() {
  SPDLOG_TRACE("++CameraManager::setDefaultCamera");
  const auto size = view_->getViewport();
  manipulator_ = CameraManipulator::Builder()
                          .targetPosition(
                              kDefaultObjectPosition.x,
                              kDefaultObjectPosition.y,
                              kDefaultObjectPosition.z
                              )
                          .viewport(static_cast<int>(size.width), static_cast<int>(size.height))
                          .build(::filament::camutils::Mode::ORBIT);
  cameraEntity_ = engine_->getEntityManager().create();
  camera_ = engine_->createCamera(cameraEntity_);
  camera_->setExposure(
        kAperture,
        kShutterSpeed,
        kSensitivity
    );
  SPDLOG_TRACE("--CameraManager::setDefaultCamera");
}

void CameraManager::updateCamera(Camera* camera) {
  SPDLOG_TRACE("++CameraManager::updateCamera");
  SPDLOG_TRACE("--CameraManager::updateCamera");
}

void CameraManager::lookAtDefaultPosition() {
  SPDLOG_TRACE("++CameraManager::lookAtDefaultPosition");
  if (manipulator_ && camera_) {
    filament::math::float3 eye, center, up;
    manipulator_->getLookAt(&eye, &center, &up);
    camera_->lookAt(eye, center, up);
  }
  SPDLOG_TRACE("--CameraManager::lookAtDefaultPosition");
}

void CameraManager::destroyCamera() {
  SPDLOG_DEBUG("++CameraManager::destroyCamera");
  engine_->destroyCameraComponent(cameraEntity_);
  utils::EntityManager::get().destroy(cameraEntity_);
  SPDLOG_DEBUG("--CameraManager::destroyCamera");
}

void CameraManager::onPointerDown(int x, int y) {
  manipulator_->grabBegin(x, y, false);
}

void CameraManager::onPointerMove(int x, int y) {
  manipulator_->grabUpdate(x, y);
}

void CameraManager::onPointerUp(int x, int y) {
  manipulator_->grabEnd();
}

}  // namespace plugin_filament_view