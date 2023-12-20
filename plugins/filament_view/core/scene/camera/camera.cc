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

#include "camera.h"

#include <memory>

#include "shell/platform/common/client_wrapper/include/flutter/encodable_value.h"

#include "logging/logging.h"
#include "utils.h"

namespace plugin_filament_view {

Camera::Camera(const flutter::EncodableMap& params) {
  SPDLOG_TRACE("++Camera::Camera");
  for (const auto& it : params) {
    if (it.second.IsNull())
      continue;

    auto key = std::get<std::string>(it.first);
    if (key == "exposure" &&
        std::holds_alternative<flutter::EncodableMap>(it.second)) {
      exposure_ = std::make_unique<Exposure>(
          std::get<flutter::EncodableMap>(it.second));
      exposure_wrapper_ = exposure_.value().get();
    } else if (key == "projection" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      projection_ = std::make_unique<Projection>(
          std::get<flutter::EncodableMap>(it.second));
      projection_wrapper_ = projection_.value().get();
    } else if (key == "lensProjection" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      lensProjection_ = std::make_unique<LensProjection>(
          std::get<flutter::EncodableMap>(it.second));
      lensProjection_wrapper_ = lensProjection_.value().get();
    } else if (key == "farPlane" && std::holds_alternative<double>(it.second)) {
      farPlane_ = std::get<double>(it.second);
    } else if (key == "flightMaxMoveSpeed" &&
               std::holds_alternative<double>(it.second)) {
      flightMaxMoveSpeed_ = std::get<double>(it.second);
    } else if (key == "flightMoveDamping" &&
               std::holds_alternative<double>(it.second)) {
      flightMoveDamping_ = std::get<double>(it.second);
    } else if (key == "flightSpeedSteps" &&
               std::holds_alternative<int64_t>(it.second)) {
      flightSpeedSteps_ = std::get<int64_t>(it.second);
    } else if (key == "flightStartOrientation" &&
               std::holds_alternative<flutter::EncodableList>(it.second)) {
      auto list = std::get<flutter::EncodableList>(it.second);
      flightStartOrientation_ = std::make_unique<std::vector<float>>();
      for (const auto& item : list) {
        flightStartOrientation_.value()->emplace_back(std::get<double>(item));
      }
    } else if (key == "flightStartPosition" &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      flightStartPosition_ = std::make_unique<Position>(
          std::get<flutter::EncodableMap>(it.second));
    } else if (key == "fovDirection" && !it.second.IsNull() &&
               std::holds_alternative<std::string>(it.second)) {
      fovDirection_ = getFovForText(std::get<std::string>(it.second));
    } else if (key == "fovDegrees" && !it.second.IsNull() &&
               std::holds_alternative<double>(it.second)) {
      fovDegrees_ = std::get<double>(it.second);
    } else if (key == "farPlane" && !it.second.IsNull() &&
               std::holds_alternative<double>(it.second)) {
      farPlane_ = std::get<double>(it.second);
    } else if (key == "groundPlane" && !it.second.IsNull() &&
               std::holds_alternative<flutter::EncodableList>(it.second)) {
      auto list = std::get<flutter::EncodableList>(it.second);
      groundPlane_ = std::make_unique<std::vector<float>>();
      for (const auto& item : list) {
        groundPlane_.value()->emplace_back(std::get<double>(item));
      }
    } else if (key == "mapExtent" && !it.second.IsNull() &&
               std::holds_alternative<flutter::EncodableList>(it.second)) {
      auto list = std::get<flutter::EncodableList>(it.second);
      mapExtent_ = std::make_unique<std::vector<float>>();
      for (const auto& item : list) {
        mapExtent_.value()->emplace_back(std::get<double>(item));
      }
    } else if (key == "mapMinDistance" && !it.second.IsNull() &&
               std::holds_alternative<double>(it.second)) {
      mapMinDistance_ = std::get<double>(it.second);
    } else if (key == "mode" && !it.second.IsNull() &&
               std::holds_alternative<std::string>(it.second)) {
      mode_ = getModeForText(std::get<std::string>(it.second));
    } else if (key == "orbitHomePosition" && !it.second.IsNull() &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      orbitHomePosition_ = std::make_unique<Position>(
          std::get<flutter::EncodableMap>(it.second));
    } else if (key == "orbitSpeed" && !it.second.IsNull() &&
               std::holds_alternative<flutter::EncodableList>(it.second)) {
      auto list = std::get<flutter::EncodableList>(it.second);
      orbitSpeed_ = std::make_unique<std::vector<float>>();
      for (const auto& item : list) {
        orbitSpeed_.value()->emplace_back(std::get<double>(item));
      }
    } else if (key == "scaling" && !it.second.IsNull() &&
               std::holds_alternative<flutter::EncodableList>(it.second)) {
      auto list = std::get<flutter::EncodableList>(it.second);
      scaling_ = std::make_unique<std::vector<double>>();
      for (const auto& item : list) {
        scaling_.value()->emplace_back(std::get<double>(item));
      }
      scaling_wrapper_ = scaling_.value().get();
    } else if (key == "shift" && !it.second.IsNull() &&
               std::holds_alternative<flutter::EncodableList>(it.second)) {
      auto list = std::get<flutter::EncodableList>(it.second);
      shift_ = std::make_unique<std::vector<double>>();
      for (const auto& item : list) {
        shift_.value()->emplace_back(std::get<double>(item));
      }
      shift_wrapper_ = shift_.value().get();
    } else if (key == "targetPosition" && !it.second.IsNull() &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      targetPosition_ = std::make_unique<Position>(
          std::get<flutter::EncodableMap>(it.second));
    } else if (key == "upVector" && !it.second.IsNull() &&
               std::holds_alternative<flutter::EncodableMap>(it.second)) {
      upVector_ = std::make_unique<Position>(
          std::get<flutter::EncodableMap>(it.second));
    } else if (key == "zoomSpeed" && !it.second.IsNull() &&
               std::holds_alternative<double>(it.second)) {
      zoomSpeed_ = std::get<double>(it.second);
    } else if (!it.second.IsNull()) {
      spdlog::debug("[Camera] Unhandled Parameter");
      Utils::PrintFlutterEncodableValue(key.c_str(), it.second);
    }
  }
  SPDLOG_TRACE("--Camera::Camera");
}

void Camera::Print(const char* tag) {
  spdlog::debug("++++++++");
  spdlog::debug("{} (Camera)", tag);
  if (exposure_.has_value()) {
    exposure_.value()->Print("\texposure");
  }
  if (projection_.has_value()) {
    projection_.value()->Print("\tprojection");
  }
  if (lensProjection_.has_value()) {
    lensProjection_.value()->Print("\tlensProjection");
  }
  if (farPlane_.has_value()) {
    spdlog::debug("\tfarPlane: {}", farPlane_.value());
  }
  if (flightMaxMoveSpeed_.has_value()) {
    spdlog::debug("\tflightMaxMoveSpeed: {}", flightMaxMoveSpeed_.value());
  }
  if (flightMoveDamping_.has_value()) {
    spdlog::debug("\tflightMoveDamping: {}", flightMoveDamping_.value());
  }
  if (flightSpeedSteps_.has_value()) {
    spdlog::debug("\tflightSpeedSteps: {}", flightSpeedSteps_.value());
  }
  if (flightStartOrientation_.has_value()) {
    for (const auto& it_ : *flightStartOrientation_.value()) {
      spdlog::debug("\tflightStartOrientation: {}", it_);
    }
  }
  if (flightStartPosition_.has_value()) {
    flightStartPosition_.value()->Print("\tflightStartPosition");
  }
  if (fovDegrees_.has_value()) {
    spdlog::debug("\tfovDegrees: {}", fovDegrees_.value());
  }
  if (fovDegrees_.has_value()) {
    spdlog::debug("\tfovDegrees: {}", fovDegrees_.value());
  }
  if (farPlane_.has_value()) {
    spdlog::debug("\tfarPlane: {}", farPlane_.value());
  }
  if (groundPlane_.has_value()) {
    for (const auto& it_ : *groundPlane_.value()) {
      spdlog::debug("\tgroundPlane: {}", it_);
    }
  }
  if (mapExtent_.has_value()) {
    for (const auto& it_ : *mapExtent_.value()) {
      spdlog::debug("\tmapExtent: {}", it_);
    }
  }
  if (mapExtent_.has_value()) {
    spdlog::debug("\tmapMinDistance: {}", mapMinDistance_.value());
  }
  spdlog::debug("\tmode: [{}]", getTextForMode(mode_));
  if (orbitHomePosition_.has_value()) {
    orbitHomePosition_.value()->Print("\torbitHomePosition");
  }
  spdlog::debug("\tfovDirection: [{}]", getTextForFov(fovDirection_));
  if (orbitSpeed_.has_value()) {
    for (const auto& it_ : *orbitSpeed_.value()) {
      spdlog::debug("\torbitSpeed: {}", it_);
    }
  }
  if (scaling_.has_value()) {
    for (const auto& it_ : *scaling_.value()) {
      spdlog::debug("\tscaling: {}", it_);
    }
  }
  if (shift_.has_value()) {
    for (const auto& it_ : *shift_.value()) {
      spdlog::debug("\tshift: {}", it_);
    }
  }
  if (targetPosition_.has_value()) {
    targetPosition_.value()->Print("\ttargetPosition");
  }
  if (upVector_.has_value()) {
    upVector_.value()->Print("\tupVector");
  }
  if (zoomSpeed_.has_value()) {
    spdlog::debug("\tzoomSpeed: {}", zoomSpeed_.value());
  }
  spdlog::debug("++++++++");
}

const char* Camera::getTextForMode(::filament::camutils::Mode mode) {
  return (const char*[]){
      kModeOrbit,
      kModeMap,
      kModeFreeFlight,
  }[static_cast<int>(mode)];
}

::filament::camutils::Mode Camera::getModeForText(const std::string& mode) {
  if (mode == kModeMap) {
    return ::filament::camutils::Mode::MAP;
  } else if (mode == kModeFreeFlight) {
    return ::filament::camutils::Mode::FREE_FLIGHT;
  }
  return ::filament::camutils::Mode::ORBIT;
}

const char* Camera::getTextForFov(::filament::camutils::Fov fov) {
  return (const char*[]){
      kFovVertical,
      kFovHorizontal,
  }[static_cast<int>(fov)];
}

::filament::camutils::Fov Camera::getFovForText(const std::string& fov) {
  if (fov == kFovVertical) {
    return ::filament::camutils::Fov::VERTICAL;
  } else if (fov == kFovHorizontal) {
    return ::filament::camutils::Fov::HORIZONTAL;
  }
  return ::filament::camutils::Fov::HORIZONTAL;
}

}  // namespace plugin_filament_view