
#include "camera_manager.h"

#include <asio/post.hpp>

#include "logging/logging.h"

namespace plugin_filament_view {
CameraManager::CameraManager(CustomModelViewer* model_viewer)
    : model_viewer_(model_viewer) {
  SPDLOG_TRACE("++CameraManager::CameraManager");
  setDefaultCamera();
  SPDLOG_TRACE("--CameraManager::CameraManager");
}

std::future<void> CameraManager::setupCamera() {
  SPDLOG_TRACE("++CameraManager::setupCamera");
  const auto promise(std::make_shared<std::promise<void>>());
  auto future(promise->get_future());

  asio::post(*model_viewer_->getStrandContext(),[&, promise]{
    auto engine = model_viewer_->getEngine();
    auto view = model_viewer_->getView();
    const auto size = view->getViewport();
    manipulator_ = CameraManipulator::Builder()
                       .targetPosition(
                           kDefaultObjectPosition.x,
                           kDefaultObjectPosition.y,
                           kDefaultObjectPosition.z
                           )
                       .viewport(static_cast<int>(size.width), static_cast<int>(size.height))
                       .build(::filament::camutils::Mode::ORBIT);
    cameraEntity_ = engine->getEntityManager().create();
    camera_ = engine->createCamera(cameraEntity_);
    camera_->setExposure(
        kAperture,
        kShutterSpeed,
        kSensitivity
    );
  });
  SPDLOG_TRACE("--CameraManager::setupCamera");
  return future;
}

void CameraManager::setDefaultCamera() {
  SPDLOG_TRACE("++CameraManager::setDefaultCamera");
  SPDLOG_TRACE("--CameraManager::setDefaultCamera");
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
  auto engine = model_viewer_->getEngine();
  engine->destroyCameraComponent(cameraEntity_);
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