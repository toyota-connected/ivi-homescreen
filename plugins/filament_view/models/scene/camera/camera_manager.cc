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

#include "camera_manager.h"

#include <asio/post.hpp>

#include "logging/logging.h"

namespace plugin_filament_view {
CameraManager::CameraManager(CustomModelViewer* model_viewer)
    : model_viewer_(model_viewer), engine_(model_viewer->getFilamentEngine()) {
  SPDLOG_TRACE("++CameraManager::CameraManager");
  auto f = setDefaultCamera();
  f.wait();
  SPDLOG_TRACE("--CameraManager::CameraManager");
}

std::future<void> CameraManager::setDefaultCamera() {
  const auto promise(std::make_shared<std::promise<void>>());
  auto future(promise->get_future());

  asio::post(*model_viewer_->getStrandContext(), [&, promise] {
    assert(model_viewer_);
    auto fview = model_viewer_->getFilamentView();
    assert(fview);
    auto viewport = fview->getViewport();

    cameraManipulator_ =
        CameraManipulator::Builder()
            .targetPosition(kDefaultObjectPosition.x, kDefaultObjectPosition.y,
                            kDefaultObjectPosition.z)
            .viewport(static_cast<int>(viewport.width),
                      static_cast<int>(viewport.height))
            .build(::filament::camutils::Mode::ORBIT);

    camera_ = engine_->createCamera(engine_->getEntityManager().create());
    camera_->setExposure(kAperture, kShutterSpeed, kSensitivity);
    promise->set_value();
  });
  return future;
}

std::string CameraManager::updateExposure(std::optional<Exposure*> exposure) {
  if (!exposure.has_value()) {
    return "Exposure not found";
  }
  auto e = exposure.value();
  if (e->getExposure().has_value()) {
    SPDLOG_DEBUG("exposure: {}", e->getExposure().value());
    camera_->setExposure(e->getExposure().value());
    return "Exposure updated successfully";
  } else {
    auto aperture =
        e->getAperture().has_value() ? e->getAperture().value() : kAperture;
    auto shutterSpeed = e->getShutterSpeed().has_value()
                            ? e->getShutterSpeed().value()
                            : kShutterSpeed;
    auto sensitivity = e->getSensitivity().has_value()
                           ? e->getSensitivity().value()
                           : kSensitivity;
    SPDLOG_DEBUG("aperture: {}", aperture);
    SPDLOG_DEBUG("shutterSpeed: {}", shutterSpeed);
    SPDLOG_DEBUG("sensitivity: {}", sensitivity);
    camera_->setExposure(aperture, shutterSpeed, sensitivity);
    return "Exposure updated successfully";
  }
  return "Exposure aperture and shutter speed and sensitivity must be provided";
}

std::string CameraManager::updateProjection(
    std::optional<Projection*> projection) {
  if (!projection.has_value()) {
    return "Projection not found";
  }
  auto p = projection.value();
  if (p->getProjection().has_value() && p->getLeft().has_value() &&
      p->getRight().has_value() && p->getTop().has_value() &&
      p->getBottom().has_value()) {
    camera_->setProjection(
        p->getProjection().value(), p->getLeft().value(), p->getRight().value(),
        p->getBottom().value(), p->getTop().value(),
        p->getNear().has_value() ? p->getNear().value() : kNearPlane,
        p->getFar().has_value() ? p->getFar().value() : kFarPlane);
    return "Projection updated successfully";
  } else if (p->getFovInDegrees().has_value() &&
             p->getDirection().has_value()) {
    auto aspect = p->getAspect().has_value() ? p->getAspect().value()
                                             : calculateAspectRatio();
    camera_->setProjection(
        p->getFovInDegrees().value(), aspect,
        p->getNear().has_value() ? p->getNear().value() : kNearPlane,
        p->getFar().has_value() ? p->getFar().value() : kFarPlane,
        p->getDirection().value());
    return "Projection updated successfully";
  } else {
    return "Projection info must be provided";
  }
}

std::string CameraManager::updateCameraShift(
    std::optional<std::vector<double>*> shift) {
  if (!shift.has_value()) {
    return "Camera shift not found";
  }
  if (shift.value()->size() >= 2) {
    return "Camera shift info must be provided";
  }
  camera_->setShift({shift.value()->at(0), shift.value()->at(1)});
  return "Camera shift updated successfully";
}

std::string CameraManager::updateCameraScaling(
    std::optional<std::vector<double>*> scaling) {
  if (!scaling.has_value()) {
    return "Camera scaling must be provided";
  }
  if (scaling.value()->size() >= 2) {
    return "Camera scaling info must be provided";
  }
  camera_->setScaling({scaling.value()->at(0), scaling.value()->at(1)});
  return "Camera scaling updated successfully";
}

void CameraManager::updateCameraManipulator(Camera* cameraInfo) {
  if (!cameraInfo) {
    return;
  }

  auto manipulatorBuilder = CameraManipulator::Builder();

  if (cameraInfo->getTargetPosition().has_value()) {
    auto tp = cameraInfo->getTargetPosition().value();
    manipulatorBuilder.targetPosition(tp->getX(), tp->getY(), tp->getZ());
  } else {
    manipulatorBuilder.targetPosition(kDefaultObjectPosition.x,
                                      kDefaultObjectPosition.y,
                                      kDefaultObjectPosition.z);
  }

  if (cameraInfo->getUpVector().has_value()) {
    auto upVector = cameraInfo->getUpVector().value();
    auto x = upVector->getX();
    auto y = upVector->getY();
    auto z = upVector->getZ();
    manipulatorBuilder.upVector(x, y, z);
  }
  if (cameraInfo->getZoomSpeed().has_value()) {
    manipulatorBuilder.zoomSpeed(cameraInfo->getZoomSpeed().value());
  }

  if (cameraInfo->getOrbitHomePosition().has_value()) {
    auto orbitHomePosition = cameraInfo->getOrbitHomePosition().value();
    auto x = orbitHomePosition->getX();
    auto y = orbitHomePosition->getY();
    auto z = orbitHomePosition->getZ();
    manipulatorBuilder.orbitHomePosition(x, y, z);
  }

  if (cameraInfo->getOrbitSpeed().has_value()) {
    auto orbitSpeed = cameraInfo->getOrbitSpeed().value();
    float x = orbitSpeed->at(0);
    float y = orbitSpeed->at(1);
    manipulatorBuilder.orbitSpeed(x, y);
  }

  if (cameraInfo->getFovDirection().has_value()) {
    manipulatorBuilder.fovDirection(cameraInfo->getFovDirection().value());
  }

  if (cameraInfo->getFovDegrees().has_value()) {
    manipulatorBuilder.fovDegrees(cameraInfo->getFovDegrees().value());
  }

  if (cameraInfo->getFarPlane().has_value()) {
    manipulatorBuilder.farPlane(cameraInfo->getFarPlane().value());
  }

  if (cameraInfo->getMapExtent().has_value()) {
    auto mapExtent = cameraInfo->getMapExtent().value();
    float width = mapExtent->at(0);
    float height = mapExtent->at(1);
    manipulatorBuilder.mapExtent(width, height);
  }

  if (cameraInfo->getFlightStartPosition().has_value()) {
    auto flightStartPosition = cameraInfo->getFlightStartPosition().value();
    auto x = flightStartPosition->getX();
    auto y = flightStartPosition->getY();
    auto z = flightStartPosition->getZ();
    manipulatorBuilder.flightStartPosition(x, y, z);
  }

  if (cameraInfo->getFlightStartOrientation().has_value()) {
    auto flightStartOrientation =
        cameraInfo->getFlightStartOrientation().value();
    // val pitch = it.getOrElse(0) { 0f }
    auto pitch = flightStartOrientation->at(0);  // 0f;
    // val yaw = it.getOrElse(1) { 0f }
    auto yaw = flightStartOrientation->at(1);  // 0f;
    manipulatorBuilder.flightStartOrientation(pitch, yaw);
  }

  if (cameraInfo->getFlightMoveDamping().has_value()) {
    manipulatorBuilder.flightMoveDamping(
        cameraInfo->getFlightMoveDamping().value());
  }

  if (cameraInfo->getFlightSpeedSteps().has_value()) {
    manipulatorBuilder.flightSpeedSteps(
        cameraInfo->getFlightSpeedSteps().value());
  }

  if (cameraInfo->getFlightMaxMoveSpeed().has_value()) {
    manipulatorBuilder.flightMaxMoveSpeed(
        cameraInfo->getFlightMaxMoveSpeed().value());
  }

  if (cameraInfo->getGroundPlane().has_value()) {
    auto groundPlane = cameraInfo->getGroundPlane().value();
    auto a = groundPlane->at(0);  //{ 0f };
    auto b = groundPlane->at(1);  //{ 0f };
    auto c = groundPlane->at(2);  //{ 1f };
    auto d = groundPlane->at(3);  //{ 0f };
    manipulatorBuilder.groundPlane(a, b, c, d);
  }

  auto viewport = model_viewer_->getFilamentView()->getViewport();
  manipulatorBuilder.viewport(static_cast<int>(viewport.width),
                              static_cast<int>(viewport.height));
  cameraManipulator_ = manipulatorBuilder.build(
      cameraInfo->getMode().has_value() ? cameraInfo->getMode().value()
                                        : ::filament::camutils::Mode::ORBIT);

#if 0
    val view = surfaceView ?: textureView
  view?.let {
  gestureDetector = GestureDetector(it, cameraManipulator)
  }
#endif
}

std::future<std::string> CameraManager::updateCamera(Camera* cameraInfo) {
  SPDLOG_DEBUG("++CameraManager::updateCamera");
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());

  assert(model_viewer_);
  if (!cameraInfo) {
    promise->set_value("Camera not found");
  } else {
    auto strand = model_viewer_->getStrandContext();
    asio::post(*strand, [&, promise, cameraInfo] {
      updateExposure(cameraInfo->getExposure());
      updateProjection(cameraInfo->getProjection());
      updateLensProjection(cameraInfo->getLensProjection());
      updateCameraShift(cameraInfo->getShift());
      updateCameraScaling(cameraInfo->getScaling());
      updateCameraManipulator(cameraInfo);
      promise->set_value("Camera updated successfully");
    });
  }

  SPDLOG_DEBUG("--CameraManager::updateCamera");
  return future;
}

void CameraManager::lookAtDefaultPosition() {
  SPDLOG_TRACE("++CameraManager::lookAtDefaultPosition");
  filament::math::float3 eye, center, up;
  cameraManipulator_->getLookAt(&eye, &center, &up);
  camera_->lookAt(eye, center, up);
  SPDLOG_TRACE("--CameraManager::lookAtDefaultPosition");
}

void CameraManager::destroyCamera() {
  SPDLOG_DEBUG("++CameraManager::destroyCamera");
  engine_->destroyCameraComponent(cameraEntity_);
  utils::EntityManager::get().destroy(cameraEntity_);
  SPDLOG_DEBUG("--CameraManager::destroyCamera");
}

void CameraManager::onAction(int32_t action, double x, double y) {
  static int prev_action = 255;  // used for edge detection
  if (action == 0) {
    if (prev_action != action) {
      prev_action = action;
      SPDLOG_TRACE("grabBegin: {}, {}", static_cast<int>(x),
                   static_cast<int>(y));
      cameraManipulator_->grabBegin(static_cast<int>(x), static_cast<int>(y),
                                    false);
    } else {
      SPDLOG_TRACE("grabUpdate: {}, {}", static_cast<int>(x),
                   static_cast<int>(y));
      cameraManipulator_->grabUpdate(static_cast<int>(x), static_cast<int>(y));
    }
  } else if (action == 1) {
    SPDLOG_TRACE("grabEnd");
    cameraManipulator_->grabEnd();
    prev_action = 255;
  }
}

std::string CameraManager::updateLensProjection(
    std::optional<LensProjection*> lensProjection) {
  if (!lensProjection.has_value()) {
    return "Lens projection not found";
  }

  if (lensProjection.value()->getFocalLength().has_value()) {
    if (cameraFocalLength_ != lensProjection.value()->getFocalLength().value())
      cameraFocalLength_ = lensProjection.value()->getFocalLength().value();
    auto aspect = lensProjection.value()->getAspect().has_value()
                      ? lensProjection.value()->getAspect().value()
                      : calculateAspectRatio();
    camera_->setLensProjection(lensProjection.value()->getFocalLength().value(),
                               aspect,
                               lensProjection.value()->getNear().has_value()
                                   ? lensProjection.value()->getNear().value()
                                   : kNearPlane,
                               lensProjection.value()->getFar().has_value()
                                   ? lensProjection.value()->getFar().value()
                                   : kFarPlane);
    return "Lens projection updated successfully";
  }
  return "Lens projection info must be provided";
}

void CameraManager::updateCameraProjection() {
  auto aspect = calculateAspectRatio();
  std::optional<LensProjection*> lensProjection =
      new LensProjection(cameraFocalLength_, aspect);
  updateLensProjection(lensProjection);
  delete lensProjection.value();
}

float CameraManager::calculateAspectRatio() {
  auto viewport = model_viewer_->getFilamentView()->getViewport();
  return static_cast<float>(viewport.width) /
         static_cast<float>(viewport.height);
}

void CameraManager::updateCameraOnResize(uint32_t width, uint32_t height) {
  cameraManipulator_->setViewport(static_cast<int>(width),
                                  static_cast<int>(height));
  updateCameraProjection();
}

}  // namespace plugin_filament_view
