/*
 * Copyright 2020-2024 Toyota Connected North America
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

#include "camera_plugin.h"

#include <memory>

#include <libcamera/libcamera.h>

#include "camera_context.h"

#include "plugins/common/common.h"

using namespace plugin_common;

namespace camera_plugin {

// TODO static constexpr char kKeyMaxVideoDuration[] = "maxVideoDuration";

// TODO static constexpr char kResolutionPresetValueLow[] = "low";
// TODO static constexpr char kResolutionPresetValueMedium[] = "medium";
// TODO static constexpr char kResolutionPresetValueHigh[] = "high";
// TODO static constexpr char kResolutionPresetValueVeryHigh[] = "veryHigh";
// TODO static constexpr char kResolutionPresetValueUltraHigh[] = "ultraHigh";
// TODO static constexpr char kResolutionPresetValueMax[] = "max";

static std::unique_ptr<libcamera::CameraManager> g_camera_manager;
static std::vector<std::shared_ptr<CameraContext>> g_cameras;

// static
void CameraPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarDesktop* registrar) {
  auto plugin =
      std::make_unique<CameraPlugin>(registrar, registrar->messenger());
  CameraPlugin::SetUp(registrar->messenger(), plugin.get());
  registrar->AddPlugin(std::move(plugin));
}

CameraPlugin::CameraPlugin(flutter::PluginRegistrarDesktop* plugin_registrar,
                           flutter::BinaryMessenger* messenger)
    : registrar_(plugin_registrar), messenger_(messenger) {
  g_camera_manager = std::make_unique<libcamera::CameraManager>();
  g_camera_manager->cameraAdded.connect(this, &CameraPlugin::camera_added);
  g_camera_manager->cameraRemoved.connect(this, &CameraPlugin::camera_removed);

  spdlog::debug("[camera_plugin] libcamera {}", g_camera_manager->version());

  auto res = g_camera_manager->start();
  if (res != 0) {
    spdlog::critical("Failed to start camera manager: {}", strerror(-res));
  }
}

CameraPlugin::~CameraPlugin() {
  g_camera_manager->stop();
}

void CameraPlugin::camera_added(const std::shared_ptr<libcamera::Camera>& cam) {
  spdlog::debug("[camera_plugin] Camera added: {}", cam->id());
}

void CameraPlugin::camera_removed(
    const std::shared_ptr<libcamera::Camera>& cam) {
  spdlog::debug("[camera_plugin] Camera removed: {}", cam->id());
  for (const auto& camera : g_cameras) {
    if (camera->getCameraId() == cam->id()) {
      switch (camera->getCameraState()) {
        case CameraContext::CAM_STATE_RUNNING:
          cam->stop();
        case CameraContext::CAM_STATE_ACQUIRED:
        case CameraContext::CAM_STATE_CONFIGURED:
          cam->release();
          break;
        default:
          break;
      }
    }
  }
}

std::string CameraPlugin::get_camera_lens_facing(
    const std::shared_ptr<libcamera::Camera>& camera) {
  const libcamera::ControlList& props = camera->properties();
  std::string lensFacing;

  // If location is specified use it, otherwise select external
  const auto& location = props.get(libcamera::properties::Location);
  if (location) {
    switch (*location) {
      case libcamera::properties::CameraLocationFront:
        lensFacing = "front";
        break;
      case libcamera::properties::CameraLocationBack:
        lensFacing = "back";
        break;
      case libcamera::properties::CameraLocationExternal:
        lensFacing = "external";
        break;
    }
  } else {
    lensFacing = "external";
  }
  return std::move(lensFacing);
}

void CameraPlugin::availableCameras(
    std::function<void(ErrorOr<flutter::EncodableList> reply)> result) {
  spdlog::debug("[camera_plugin] availableCameras:");

  auto cameras = g_camera_manager->cameras();
  flutter::EncodableList list;
  for (auto const& camera : cameras) {
    std::string id = camera->id();
    std::string lensFacing = get_camera_lens_facing(camera);
    int64_t sensorOrientation = 0;
    spdlog::debug("\tid: {}", id);
    spdlog::debug("\tlensFacing: {}", lensFacing);
    spdlog::debug("\tsensorOrientation: {}", sensorOrientation);
    list.emplace_back(
        flutter::EncodableMap{{flutter::EncodableValue("name"),
                               flutter::EncodableValue(std::move(id))},
                              {flutter::EncodableValue("lensFacing"),
                               flutter::EncodableValue(std::move(lensFacing))},
                              {flutter::EncodableValue("sensorOrientation"),
                               flutter::EncodableValue(sensorOrientation)}});
  }
  result(list);
}

void CameraPlugin::create(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<flutter::EncodableMap> reply)> result) {
  spdlog::debug("[camera_plugin] create:");
  plugin_common::Encodable::PrintFlutterEncodableMap("create", args);

  // method arguments
  std::string cameraName;
  std::string resolutionPreset;
  int64_t fps = 0;
  int64_t videoBitrate = 0;
  int64_t audioBitrate = 0;
  bool enableAudio{};

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraName" && std::holds_alternative<std::string>(it.second)) {
      cameraName = std::get<std::string>(it.second);
    } else if (key == "resolutionPreset" &&
               std::holds_alternative<std::string>(it.second)) {
      resolutionPreset = std::get<std::string>(it.second);
    } else if (key == "fps" && std::holds_alternative<int64_t>(it.second)) {
      fps = std::get<int64_t>(it.second);
    } else if (key == "videoBitrate" &&
               std::holds_alternative<int64_t>(it.second)) {
      videoBitrate = std::get<int64_t>(it.second);
    } else if (key == "audioBitrate" &&
               std::holds_alternative<int64_t>(it.second)) {
      audioBitrate = std::get<int64_t>(it.second);
    } else if (key == "enableAudio" &&
               std::holds_alternative<bool>(it.second)) {
      enableAudio = std::get<bool>(it.second);
    }
  }
  // Create Camera instance
  auto camera = std::make_shared<CameraContext>(
      cameraName, resolutionPreset, fps, videoBitrate, audioBitrate,
      enableAudio, g_camera_manager->get(cameraName));
  g_cameras.emplace_back(std::move(camera));

  result(flutter::EncodableMap(
      {{flutter::EncodableValue(std::string("cameraId")),
        flutter::EncodableValue(static_cast<int64_t>(g_cameras.size()))}}));
}

void CameraPlugin::initialize(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<std::string> reply)> result) {
  // method arguments
  int32_t cameraId = 0;
  std::string imageFormatGroup;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    } else if (key == "imageFormatGroup" &&
               std::holds_alternative<std::string>(it.second)) {
      imageFormatGroup = std::get<std::string>(it.second);
    }
  }

  // Initialize Camera
  if (cameraId - 1 < g_cameras.size()) {
    auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
    if (!camera) {
      spdlog::error("Invalid cameraId");
      result(ErrorOr<std::string>("Invalid cameraId"));
      return;
    }
    auto id = camera->getCameraId();

    camera->setCamera(g_camera_manager->get(id));
    auto channel_name =
        camera->Initialize(registrar_, cameraId, imageFormatGroup);
    result(channel_name);
  }
}

void CameraPlugin::takePicture(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<std::string> reply)> result) {
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }

  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  result(camera->takePicture());
}

void CameraPlugin::startVideoRecording(
    const flutter::EncodableMap& args,
    std::function<void(std::optional<FlutterError> reply)> result) {
  plugin_common::Encodable::PrintFlutterEncodableMap("startVideoRecording",
                                                     args);
  // method arguments
  int32_t cameraId = 0;
  bool enableStream{};

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    } else if (key == "enableStream" &&
               std::holds_alternative<bool>(it.second)) {
      enableStream = std::get<bool>(it.second);
    }
  }

  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  camera->startVideoRecording(enableStream);

  result(std::nullopt);
}

void CameraPlugin::pauseVideoRecording(
    const flutter::EncodableMap& args,
    std::function<void(std::optional<FlutterError> reply)> result) {
  plugin_common::Encodable::PrintFlutterEncodableMap("pauseVideoRecording",
                                                     args);
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }

  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  camera->pauseVideoRecording();

  result(std::nullopt);
}

void CameraPlugin::resumeVideoRecording(
    const flutter::EncodableMap& args,
    std::function<void(std::optional<FlutterError> reply)> result) {
  plugin_common::Encodable::PrintFlutterEncodableMap("resumeVideoRecording",
                                                     args);
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }

  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  camera->resumeVideoRecording();

  result(std::nullopt);
}

void CameraPlugin::stopVideoRecording(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<std::string> reply)> result) {
  plugin_common::Encodable::PrintFlutterEncodableMap("stopVideoRecording",
                                                     args);
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }

  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  result(camera->stopVideoRecording());
}

void CameraPlugin::pausePreview(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<double> reply)> result) {
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }

  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  SPDLOG_DEBUG("[camera_plugin] pausePreview: camera_id: {}", cameraId);
  result(1);
}

void CameraPlugin::resumePreview(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<double> reply)> result) {
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }

  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  SPDLOG_DEBUG("[camera_plugin] resumePreview: camera_id: {}", cameraId);
  result(1);
}

void CameraPlugin::lockCaptureOrientation(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<std::string>)> result) {
  // method arguments
  int32_t cameraId = 0;
  std::string orientation;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    } else if (key == "orientation" &&
               std::holds_alternative<std::string>(it.second)) {
      orientation = std::get<std::string>(it.second);
    }
  }
  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  SPDLOG_DEBUG(
      "[camera_plugin] lockCaptureOrientation: camera_id: {}, orientation: {}",
      cameraId, orientation);
  result(orientation);
}

void CameraPlugin::unlockCaptureOrientation(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<std::string>)> result) {
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }
  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  SPDLOG_DEBUG("[camera_plugin] unlockCaptureOrientation: camera_id: {}",
               cameraId);
  std::string res;
  result(res);
}

void CameraPlugin::setFlashMode(
    const flutter::EncodableMap& args,
    std::function<void(std::optional<FlutterError> reply)> result) {
  // method arguments
  int32_t cameraId = 0;
  std::string mode;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    } else if (key == "mode" &&
               std::holds_alternative<std::string>(it.second)) {
      mode = std::get<std::string>(it.second);
    }
  }
  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  SPDLOG_DEBUG("[camera_plugin] setFlashMode: camera_id: {}, orientation: {}",
               cameraId, mode);

  result(std::nullopt);
}

void CameraPlugin::setFocusMode(
    const flutter::EncodableMap& args,
    std::function<void(std::optional<FlutterError> reply)> result) {
  // method arguments
  int32_t cameraId = 0;
  std::string mode;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    } else if (key == "mode" &&
               std::holds_alternative<std::string>(it.second)) {
      mode = std::get<std::string>(it.second);
    }
  }
  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  SPDLOG_DEBUG("[camera_plugin] setFocusMode: camera_id: {}, mode: {}",
               cameraId, mode);

  result(std::nullopt);
}

void CameraPlugin::setExposureMode(
    const flutter::EncodableMap& args,
    std::function<void(std::optional<FlutterError> reply)> result) {
  plugin_common::Encodable::PrintFlutterEncodableMap("setExposureMode", args);

  result(std::nullopt);
}

void CameraPlugin::setExposurePoint(
    const flutter::EncodableMap& args,
    std::function<void(std::optional<FlutterError> reply)> result) {
  plugin_common::Encodable::PrintFlutterEncodableMap("setExposurePoint", args);

  result(std::nullopt);
}

void CameraPlugin::setFocusPoint(
    const flutter::EncodableMap& args,
    std::function<void(std::optional<FlutterError> reply)> result) {
  plugin_common::Encodable::PrintFlutterEncodableMap("setFocusPoint", args);
  result(std::nullopt);
}

void CameraPlugin::setExposureOffset(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<double> reply)> result) {
  // method arguments
  int32_t cameraId = 0;
  double offset;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    } else if (key == "offset" && std::holds_alternative<double>(it.second)) {
      offset = std::get<double>(it.second);
    }
  }
  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  SPDLOG_DEBUG("[camera_plugin] setExposureOffset: camera_id: {}, offset: {}",
               cameraId, offset);
  result(offset);
}

void CameraPlugin::getExposureOffsetStepSize(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<double> reply)> result) {
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }
  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  SPDLOG_DEBUG("[camera_plugin] getExposureOffsetStepSize: camera_id: {}",
               cameraId);

  result(2);
}

void CameraPlugin::getMinExposureOffset(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<double> reply)> result) {
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }
  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  SPDLOG_DEBUG("[camera_plugin] getMinExposureOffset: camera_id: {}", cameraId);
  result(0);
}

void CameraPlugin::getMaxExposureOffset(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<double> reply)> result) {
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }
  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  SPDLOG_DEBUG("[camera_plugin] getMaxExposureOffset: camera_id: {}", cameraId);
  result(64);
}

void CameraPlugin::getMaxZoomLevel(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<double> reply)> result) {
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }
  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  SPDLOG_DEBUG("[camera_plugin] getMaxZoomLevel: camera_id: {}", cameraId);
  result(32);
}

void CameraPlugin::getMinZoomLevel(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<double> reply)> result) {
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }
  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  SPDLOG_DEBUG("[camera_plugin] getMinZoomLevel: camera_id: {}", cameraId);

  result(0);
}

void CameraPlugin::dispose(
    const flutter::EncodableMap& args,
    std::function<void(std::optional<FlutterError> reply)> result) {
  // method arguments
  int32_t cameraId = 0;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == "cameraId" && std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    }
  }
  auto camera = g_cameras[static_cast<unsigned long>(cameraId - 1)];
  camera.reset();
  SPDLOG_DEBUG("[camera_plugin] dispose: {}", cameraId);
  result(std::nullopt);
}

}  // namespace camera_plugin