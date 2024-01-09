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

#include "asio/post.hpp"

#include "plugins/common/common.h"

namespace plugin_filament_view {
CameraManager::CameraManager(CustomModelViewer* modelViewer)
    : modelViewer_(modelViewer), engine_(modelViewer->getFilamentEngine()) {
  SPDLOG_TRACE("++CameraManager::CameraManager");
  auto f = setDefaultCamera();
  SPDLOG_TRACE("--CameraManager::CameraManager: {}");
}

std::future<void> CameraManager::setDefaultCamera() {
  SPDLOG_TRACE("++CameraManager::setDefaultCamera");
  const auto promise(std::make_shared<std::promise<void>>());
  auto future(promise->get_future());
  asio::post(modelViewer_->getStrandContext(), [&, promise] {
    assert(modelViewer_);
    auto fview = modelViewer_->getFilamentView();
    assert(fview);

    cameraEntity_ = engine_->getEntityManager().create();
    camera_ = engine_->createCamera(cameraEntity_);

    /// With the default parameters, the scene must contain at least one Light
    /// of intensity similar to the sun (e.g.: a 100,000 lux directional light).
    camera_->setExposure(kAperture, kShutterSpeed, kSensitivity);

    auto viewport = fview->getViewport();
    cameraManipulator_ = CameraManipulator::Builder()
                             .viewport(static_cast<int>(viewport.width),
                                       static_cast<int>(viewport.height))
                             .build(::filament::camutils::Mode::ORBIT);
    filament::math::float3 eye, center, up;
    cameraManipulator_->getLookAt(&eye, &center, &up);
    camera_->lookAt(eye, center, up);
    fview->setCamera(camera_);
    SPDLOG_TRACE("--CameraManager::setDefaultCamera");
    promise->set_value();
  });
  return future;
}

std::string CameraManager::updateExposure(Exposure* exposure) {
  if (!exposure) {
    return "Exposure not found";
  }
  auto e = exposure;
  if (e->exposure_.has_value()) {
    SPDLOG_DEBUG("[setExposure] exposure: {}", e->exposure_.value());
    camera_->setExposure(e->exposure_.value());
    return "Exposure updated successfully";
  } else {
    auto aperture = e->aperture_.has_value() ? e->aperture_.value() : kAperture;
    auto shutterSpeed =
        e->shutterSpeed_.has_value() ? e->shutterSpeed_.value() : kShutterSpeed;
    auto sensitivity =
        e->sensitivity_.has_value() ? e->sensitivity_.value() : kSensitivity;
    SPDLOG_DEBUG(
        "[setExposure] aperture: {}, shutterSpeed: {}, sensitivity: {}",
        aperture, shutterSpeed, sensitivity);
    camera_->setExposure(aperture, shutterSpeed, sensitivity);
    return "Exposure updated successfully";
  }
}

std::string CameraManager::updateProjection(Projection* projection) {
  if (!projection) {
    return "Projection not found";
  }
  auto p = projection;
  if (p->projection_.has_value() && p->left_.has_value() &&
      p->right_.has_value() && p->top_.has_value() && p->bottom_.has_value()) {
    auto project = p->projection_.value();
    auto left = p->left_.value();
    auto right = p->right_.value();
    auto top = p->top_.value();
    auto bottom = p->bottom_.value();
    auto near = p->near_.has_value() ? p->near_.value() : kNearPlane;
    auto far = p->far_.has_value() ? p->far_.value() : kFarPlane;
    SPDLOG_DEBUG(
        "[setProjection] left: {}, right: {}, bottom: {}, top: {}, near: {}, "
        "far: {}",
        left, right, bottom, top, near, far);
    camera_->setProjection(project, left, right, bottom, top, near, far);
    return "Projection updated successfully";
  } else if (p->fovInDegrees_.has_value() && p->fovDirection_.has_value()) {
    auto fovInDegrees = p->fovInDegrees_.value();
    auto aspect =
        p->aspect_.has_value() ? p->aspect_.value() : calculateAspectRatio();
    auto near = p->near_.has_value() ? p->near_.value() : kNearPlane;
    auto far = p->far_.has_value() ? p->far_.value() : kFarPlane;
    auto fovDirection = p->fovDirection_.value();
    SPDLOG_DEBUG(
        "[setProjection] fovInDegress: {}, aspect: {}, near: {}, far: {}, "
        "direction: {}",
        fovInDegrees, aspect, near, far,
        Projection::getTextForFov(fovDirection));

    camera_->setProjection(fovInDegrees, aspect, near, far, fovDirection);
    return "Projection updated successfully";
  } else {
    return "Projection info must be provided";
  }
}

std::string CameraManager::updateCameraShift(std::vector<double>* shift) {
  if (!shift) {
    return "Camera shift not found";
  }
  auto s = shift;
  if (s->size() >= 2) {
    return "Camera shift info must be provided";
  }
  SPDLOG_DEBUG("[setShift] {}, {}", s->at(0), s->at(1));
  camera_->setShift({s->at(0), s->at(1)});
  return "Camera shift updated successfully";
}

std::string CameraManager::updateCameraScaling(std::vector<double>* scaling) {
  if (!scaling) {
    return "Camera scaling must be provided";
  }
  auto s = scaling;
  if (s->size() >= 2) {
    return "Camera scaling info must be provided";
  }
  SPDLOG_DEBUG("[setScaling] {}, {}", s->at(0), s->at(1));
  camera_->setScaling({s->at(0), s->at(1)});
  return "Camera scaling updated successfully";
}

void CameraManager::updateCameraManipulator(Camera* cameraInfo) {
  if (!cameraInfo) {
    return;
  }

  auto manipulatorBuilder = CameraManipulator::Builder();

  if (cameraInfo->targetPosition_) {
    auto tp = cameraInfo->targetPosition_.get();
    manipulatorBuilder.targetPosition(tp->x_, tp->y_, tp->z_);
    SPDLOG_DEBUG("[CameraManipulator] targetPosition: {}, {}, {}", tp->x_,
                 tp->y_, tp->z_);
  } else {
    manipulatorBuilder.targetPosition(kDefaultObjectPosition.x,
                                      kDefaultObjectPosition.y,
                                      kDefaultObjectPosition.z);
    SPDLOG_DEBUG("[CameraManipulator] targetPosition: {}, {}, {}",
                 kDefaultObjectPosition.x, kDefaultObjectPosition.y,
                 kDefaultObjectPosition.z);
  }

  if (cameraInfo->upVector_) {
    auto upVector = cameraInfo->upVector_.get();
    manipulatorBuilder.upVector(upVector->x_, upVector->y_, upVector->z_);
    SPDLOG_DEBUG("[CameraManipulator] upVector: {}, {}, {}", upVector->x_,
                 upVector->y_, upVector->z_);
  }
  if (cameraInfo->zoomSpeed_.has_value()) {
    manipulatorBuilder.zoomSpeed(cameraInfo->zoomSpeed_.value());
    SPDLOG_DEBUG("[CameraManipulator] zoomSpeed: {}",
                 cameraInfo->zoomSpeed_.value());
  }

  if (cameraInfo->orbitHomePosition_) {
    auto orbitHomePosition = cameraInfo->orbitHomePosition_.get();
    manipulatorBuilder.orbitHomePosition(
        orbitHomePosition->x_, orbitHomePosition->y_, orbitHomePosition->z_);
    SPDLOG_DEBUG("[CameraManipulator] orbitHomePosition: {}, {}, {}",
                 orbitHomePosition->x_, orbitHomePosition->y_,
                 orbitHomePosition->z_);
  }

  if (cameraInfo->orbitSpeed_) {
    auto orbitSpeed = cameraInfo->orbitSpeed_.get();
    manipulatorBuilder.orbitSpeed(orbitSpeed->at(0), orbitSpeed->at(1));
    SPDLOG_DEBUG("[CameraManipulator] orbitSpeed: {}, {}", orbitSpeed->at(0),
                 orbitSpeed->at(1));
  }

  manipulatorBuilder.fovDirection(cameraInfo->fovDirection_);
  SPDLOG_DEBUG("[CameraManipulator] fovDirection: {}",
               (int)cameraInfo->fovDirection_);

  if (cameraInfo->fovDegrees_.has_value()) {
    manipulatorBuilder.fovDegrees(cameraInfo->fovDegrees_.value());
    SPDLOG_DEBUG("[CameraManipulator] fovDegrees: {}",
                 cameraInfo->fovDegrees_.value());
  }

  if (cameraInfo->farPlane_.has_value()) {
    manipulatorBuilder.farPlane(cameraInfo->farPlane_.value());
    SPDLOG_DEBUG("[CameraManipulator] farPlane: {}",
                 cameraInfo->farPlane_.value());
  }

  if (cameraInfo->mapExtent_) {
    auto mapExtent = cameraInfo->mapExtent_.get();
    manipulatorBuilder.mapExtent(mapExtent->at(0), mapExtent->at(1));
    SPDLOG_DEBUG("[CameraManipulator] mapExtent: {}, {}", mapExtent->at(0),
                 mapExtent->at(1));
  }

  if (cameraInfo->flightStartPosition_) {
    auto flightStartPosition = cameraInfo->flightStartPosition_.get();
    manipulatorBuilder.flightStartPosition(flightStartPosition->x_,
                                           flightStartPosition->y_,
                                           flightStartPosition->z_);
    SPDLOG_DEBUG("[CameraManipulator] flightStartPosition: {}, {}, {}",
                 flightStartPosition->x_, flightStartPosition->y_,
                 flightStartPosition->z_);
  }

  if (cameraInfo->flightStartOrientation_) {
    auto flightStartOrientation = cameraInfo->flightStartOrientation_.get();
    // val pitch = it.getOrElse(0) { 0f }
    auto pitch = flightStartOrientation->at(0);  // 0f;
    // val yaw = it.getOrElse(1) { 0f }
    auto yaw = flightStartOrientation->at(1);  // 0f;
    manipulatorBuilder.flightStartOrientation(pitch, yaw);
    SPDLOG_DEBUG("[CameraManipulator] flightStartOrientation: {}, {}", pitch,
                 yaw);
  }

  if (cameraInfo->flightMoveDamping_.has_value()) {
    manipulatorBuilder.flightMoveDamping(
        cameraInfo->flightMoveDamping_.value());
    SPDLOG_DEBUG("[CameraManipulator] flightMoveDamping: {}",
                 cameraInfo->flightMoveDamping_.value());
  }

  if (cameraInfo->flightSpeedSteps_.has_value()) {
    manipulatorBuilder.flightSpeedSteps(cameraInfo->flightSpeedSteps_.value());
    SPDLOG_DEBUG("[CameraManipulator] flightSpeedSteps: {}",
                 cameraInfo->flightSpeedSteps_.value());
  }

  if (cameraInfo->flightMaxMoveSpeed_.has_value()) {
    manipulatorBuilder.flightMaxMoveSpeed(
        cameraInfo->flightMaxMoveSpeed_.value());
    SPDLOG_DEBUG("[CameraManipulator] flightMaxMoveSpeed: {}",
                 cameraInfo->flightMaxMoveSpeed_.value());
  }

  if (cameraInfo->groundPlane_) {
    auto groundPlane = cameraInfo->groundPlane_.get();
    auto a = groundPlane->at(0);  //{ 0f };
    auto b = groundPlane->at(1);  //{ 0f };
    auto c = groundPlane->at(2);  //{ 1f };
    auto d = groundPlane->at(3);  //{ 0f };
    manipulatorBuilder.groundPlane(a, b, c, d);
    SPDLOG_DEBUG("[CameraManipulator] flightMaxMoveSpeed: {}, {}, {}, {}", a, b,
                 c, d);
  }

  auto viewport = modelViewer_->getFilamentView()->getViewport();
  manipulatorBuilder.viewport(static_cast<int>(viewport.width),
                              static_cast<int>(viewport.height));
  cameraManipulator_ = manipulatorBuilder.build(cameraInfo->mode_);
}

std::future<Resource<std::string_view>> CameraManager::updateCamera(
    Camera* cameraInfo) {
  SPDLOG_DEBUG("++CameraManager::updateCamera");
  const auto promise(
      std::make_shared<std::promise<Resource<std::string_view>>>());
  auto future(promise->get_future());

  if (!cameraInfo) {
    promise->set_value(Resource<std::string_view>::Error("Camera not found"));
  } else {
    asio::post(modelViewer_->getStrandContext(), [&, promise, cameraInfo] {
      updateExposure(cameraInfo->exposure_.get());
      updateProjection(cameraInfo->projection_.get());
      updateLensProjection(cameraInfo->lensProjection_.get());
      updateCameraShift(cameraInfo->shift_.get());
      updateCameraScaling(cameraInfo->scaling_.get());
      updateCameraManipulator(cameraInfo);
      promise->set_value(
          Resource<std::string_view>::Success("Camera updated successfully"));
    });
  }

  SPDLOG_DEBUG("--CameraManager::updateCamera");
  return future;
}

void CameraManager::lookAtDefaultPosition() {
  // SPDLOG_TRACE("++CameraManager::lookAtDefaultPosition");
  filament::math::float3 eye, center, up;
  cameraManipulator_->getLookAt(&eye, &center, &up);
  camera_->lookAt(eye, center, up);
  // SPDLOG_TRACE("--CameraManager::lookAtDefaultPosition");
}

void CameraManager::destroyCamera() {
  SPDLOG_DEBUG("++CameraManager::destroyCamera");
  engine_->destroyCameraComponent(cameraEntity_);
  SPDLOG_DEBUG("--CameraManager::destroyCamera");
}

void CameraManager::onAction(int32_t action, double x, double y) {
  switch (action) {
    case 0:
      cameraManipulator_->grabBegin(static_cast<int>(x), static_cast<int>(y),
                                    false);
      break;
    case 2:
      cameraManipulator_->grabUpdate(static_cast<int>(x), static_cast<int>(y));
      break;
    case 1:
    default:
      cameraManipulator_->grabEnd();
      break;
  }
}

std::string CameraManager::updateLensProjection(
    LensProjection* lensProjection) {
  if (!lensProjection) {
    return "Lens projection not found";
  }

  if (lensProjection->getFocalLength().has_value()) {
    if (cameraFocalLength_ != lensProjection->getFocalLength().value())
      cameraFocalLength_ = lensProjection->getFocalLength().value();
    auto aspect = lensProjection->getAspect().has_value()
                      ? lensProjection->getAspect().value()
                      : calculateAspectRatio();
    camera_->setLensProjection(lensProjection->getFocalLength().value(), aspect,
                               lensProjection->getNear().has_value()
                                   ? lensProjection->getNear().value()
                                   : kNearPlane,
                               lensProjection->getFar().has_value()
                                   ? lensProjection->getFar().value()
                                   : kFarPlane);
    return "Lens projection updated successfully";
  }
  return "Lens projection info must be provided";
}

void CameraManager::updateCameraProjection() {
  auto aspect = calculateAspectRatio();
  auto lensProjection = new LensProjection(cameraFocalLength_, aspect);
  updateLensProjection(lensProjection);
  delete lensProjection;
}

float CameraManager::calculateAspectRatio() {
  auto viewport = modelViewer_->getFilamentView()->getViewport();
  return static_cast<float>(viewport.width) /
         static_cast<float>(viewport.height);
}

void CameraManager::updateCameraOnResize(uint32_t width, uint32_t height) {
  cameraManipulator_->setViewport(static_cast<int>(width),
                                  static_cast<int>(height));
  updateCameraProjection();
}

}  // namespace plugin_filament_view
