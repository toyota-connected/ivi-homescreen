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

  asio::post(model_viewer_->getStrandContext(), [&, promise] {
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
  return "Exposure aperture and shutter speed and sensitivity must be provided";
}

std::string CameraManager::updateProjection(
    std::optional<Projection*> projection) {
  if (!projection.has_value()) {
    return "Projection not found";
  }
  auto p = projection.value();
  if (p->projection_.has_value() && p->left_.has_value() &&
      p->right_.has_value() && p->top_.has_value() && p->bottom_.has_value()) {
    SPDLOG_DEBUG(
        "[setProjection] left: {}, right: {}, bottom: {}, top: {}, near: {}, "
        "far: {}",
        p->left_.value(), p->right_.value(), p->bottom_.value(),
        p->top_.value(), p->near_.has_value() ? p->near_.value() : kNearPlane,
        p->far_.has_value() ? p->far_.value() : kFarPlane);

    camera_->setProjection(p->projection_.value(), p->left_.value(),
                           p->right_.value(), p->bottom_.value(),
                           p->top_.value(),
                           p->near_.has_value() ? p->near_.value() : kNearPlane,
                           p->far_.has_value() ? p->far_.value() : kFarPlane);
    return "Projection updated successfully";
  } else if (p->fovInDegrees_.has_value() && p->fovDirection_.has_value()) {
    auto aspect =
        p->aspect_.has_value() ? p->aspect_.value() : calculateAspectRatio();
    SPDLOG_DEBUG(
        "[setProjection] fovInDegress: {}, aspect: {}, near: {}, far: {}, "
        "direction: {}",
        p->fovInDegrees_.value(), aspect,
        p->near_.has_value() ? p->near_.value() : kNearPlane,
        p->far_.has_value() ? p->far_.value() : kFarPlane,
        Projection::getTextForFov(p->fovDirection_.value()));
    camera_->setProjection(p->fovInDegrees_.value(), aspect,
                           p->near_.has_value() ? p->near_.value() : kNearPlane,
                           p->far_.has_value() ? p->far_.value() : kFarPlane,
                           p->fovDirection_.value());
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
  auto s = shift.value();
  if (s->size() >= 2) {
    return "Camera shift info must be provided";
  }
  SPDLOG_DEBUG("[setShift] {}, {}", s->at(0), s->at(1));
  camera_->setShift({s->at(0), s->at(1)});
  return "Camera shift updated successfully";
}

std::string CameraManager::updateCameraScaling(
    std::optional<std::vector<double>*> scaling) {
  if (!scaling.has_value()) {
    return "Camera scaling must be provided";
  }
  auto s = scaling.value();
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

  if (cameraInfo->targetPosition_.has_value()) {
    auto tp = cameraInfo->targetPosition_.value().get();
    manipulatorBuilder.targetPosition(tp->x_, tp->y_, tp->z_);
  } else {
    manipulatorBuilder.targetPosition(kDefaultObjectPosition.x,
                                      kDefaultObjectPosition.y,
                                      kDefaultObjectPosition.z);
  }

  if (cameraInfo->upVector_.has_value()) {
    auto upVector = cameraInfo->upVector_.value().get();
    manipulatorBuilder.upVector(upVector->x_, upVector->y_, upVector->z_);
  }
  if (cameraInfo->zoomSpeed_.has_value()) {
    manipulatorBuilder.zoomSpeed(cameraInfo->zoomSpeed_.value());
  }

  if (cameraInfo->orbitHomePosition_.has_value()) {
    auto orbitHomePosition = cameraInfo->orbitHomePosition_.value().get();
    manipulatorBuilder.orbitHomePosition(
        orbitHomePosition->x_, orbitHomePosition->y_, orbitHomePosition->z_);
  }

  if (cameraInfo->orbitSpeed_.has_value()) {
    auto orbitSpeed = cameraInfo->orbitSpeed_.value().get();
    manipulatorBuilder.orbitSpeed(orbitSpeed->at(0), orbitSpeed->at(1));
  }

  manipulatorBuilder.fovDirection(cameraInfo->fovDirection_);

  if (cameraInfo->fovDegrees_.has_value()) {
    manipulatorBuilder.fovDegrees(cameraInfo->fovDegrees_.value());
  }

  if (cameraInfo->farPlane_.has_value()) {
    manipulatorBuilder.farPlane(cameraInfo->farPlane_.value());
  }

  if (cameraInfo->mapExtent_.has_value()) {
    auto mapExtent = cameraInfo->mapExtent_.value().get();
    manipulatorBuilder.mapExtent(mapExtent->at(0), mapExtent->at(1));
  }

  if (cameraInfo->flightStartPosition_.has_value()) {
    auto flightStartPosition = cameraInfo->flightStartPosition_.value().get();
    manipulatorBuilder.flightStartPosition(flightStartPosition->x_,
                                           flightStartPosition->y_,
                                           flightStartPosition->z_);
  }

  if (cameraInfo->flightStartOrientation_.has_value()) {
    auto flightStartOrientation =
        cameraInfo->flightStartOrientation_.value().get();
    // val pitch = it.getOrElse(0) { 0f }
    auto pitch = flightStartOrientation->at(0);  // 0f;
    // val yaw = it.getOrElse(1) { 0f }
    auto yaw = flightStartOrientation->at(1);  // 0f;
    manipulatorBuilder.flightStartOrientation(pitch, yaw);
  }

  if (cameraInfo->flightMoveDamping_.has_value()) {
    manipulatorBuilder.flightMoveDamping(
        cameraInfo->flightMoveDamping_.value());
  }

  if (cameraInfo->flightSpeedSteps_.has_value()) {
    manipulatorBuilder.flightSpeedSteps(cameraInfo->flightSpeedSteps_.value());
  }

  if (cameraInfo->flightMaxMoveSpeed_.has_value()) {
    manipulatorBuilder.flightMaxMoveSpeed(
        cameraInfo->flightMaxMoveSpeed_.value());
  }

  if (cameraInfo->groundPlane_.has_value()) {
    auto groundPlane = cameraInfo->groundPlane_.value().get();
    auto a = groundPlane->at(0);  //{ 0f };
    auto b = groundPlane->at(1);  //{ 0f };
    auto c = groundPlane->at(2);  //{ 1f };
    auto d = groundPlane->at(3);  //{ 0f };
    manipulatorBuilder.groundPlane(a, b, c, d);
  }

  auto viewport = model_viewer_->getFilamentView()->getViewport();
  manipulatorBuilder.viewport(static_cast<int>(viewport.width),
                              static_cast<int>(viewport.height));
  cameraManipulator_ = manipulatorBuilder.build(cameraInfo->mode_);
}

std::future<std::string> CameraManager::updateCamera(Camera* cameraInfo) {
  SPDLOG_DEBUG("++CameraManager::updateCamera");
  const auto promise(std::make_shared<std::promise<std::string>>());
  auto future(promise->get_future());

  assert(model_viewer_);
  if (!cameraInfo) {
    promise->set_value("Camera not found");
  } else {
    asio::post(model_viewer_->getStrandContext(), [&, promise, cameraInfo] {
      updateExposure(cameraInfo->exposure_wrapper_);
      updateProjection(cameraInfo->projection_wrapper_);
      updateLensProjection(cameraInfo->lensProjection_wrapper_);
      updateCameraShift(cameraInfo->shift_wrapper_);
      updateCameraScaling(cameraInfo->scaling_wrapper_);
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
