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

static constexpr char kKeyCameraId[] = "cameraId";
static constexpr char kKeyName[] = "name";
static constexpr char kKeyLensFacing[] = "lensFacing";
static constexpr char kKeySensorOrientation[] = "sensorOrientation";
static constexpr char kKeyCameraName[] = "cameraName";
static constexpr char kKeyResolutionPreset[] = "resolutionPreset";
static constexpr char kKeyFps[] = "fps";
static constexpr char kKeyVideoBitrate[] = "videoBitrate";
static constexpr char kKeyAudioBitrate[] = "audioBitrate";
static constexpr char kKeyEnableAudio[] = "enableAudio";
static constexpr char kKeyImageFormatGroup[] = "imageFormatGroup";

//TODO static constexpr char kKeyMaxVideoDuration[] = "maxVideoDuration";

//TODO static constexpr char kResolutionPresetValueLow[] = "low";
//TODO static constexpr char kResolutionPresetValueMedium[] = "medium";
//TODO static constexpr char kResolutionPresetValueHigh[] = "high";
//TODO static constexpr char kResolutionPresetValueVeryHigh[] = "veryHigh";
//TODO static constexpr char kResolutionPresetValueUltraHigh[] = "ultraHigh";
//TODO static constexpr char kResolutionPresetValueMax[] = "max";

static constexpr char kPictureCaptureExtension[] = "jpeg";
static constexpr char kVideoCaptureExtension[] = "mp4";

static std::unique_ptr<libcamera::CameraManager> g_camera_manager;
static std::vector<std::shared_ptr<CameraContext>> g_cameras;

// static
void CameraPlugin::RegisterWithRegistrar(flutter::PluginRegistrar* registrar) {
  auto plugin = std::make_unique<CameraPlugin>(registrar->texture_registrar(),
                                               registrar->messenger());
  CameraPlugin::SetUp(registrar->messenger(), plugin.get());
  registrar->AddPlugin(std::move(plugin));
}

CameraPlugin::CameraPlugin(flutter::TextureRegistrar* texture_registrar,
                           flutter::BinaryMessenger* messenger)
    : texture_registrar_(texture_registrar), messenger_(messenger) {
  g_camera_manager = std::make_unique<libcamera::CameraManager>();
  g_camera_manager->cameraAdded.connect(this, &CameraPlugin::camera_added);
  g_camera_manager->cameraRemoved.connect(this, &CameraPlugin::camera_removed);

  spdlog::debug("\tlibcamera {}", g_camera_manager->version());

  auto res = g_camera_manager->start();
  if (res != 0) {
    spdlog::critical("Failed to start camera manager: {}", strerror(-res));
  }
}

CameraPlugin::~CameraPlugin() {
  g_camera_manager->stop();
}

void CameraPlugin::camera_added(const std::shared_ptr<libcamera::Camera>& cam) {
  spdlog::debug("Camera added: {}", cam->id());
}

void CameraPlugin::camera_removed(
    const std::shared_ptr<libcamera::Camera>& cam) {
  spdlog::debug("Camera removed: {}", cam->id());
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
        flutter::EncodableMap{{flutter::EncodableValue(kKeyName),
                               flutter::EncodableValue(std::move(id))},
                              {flutter::EncodableValue(kKeyLensFacing),
                               flutter::EncodableValue(std::move(lensFacing))},
                              {flutter::EncodableValue(kKeySensorOrientation),
                               flutter::EncodableValue(sensorOrientation)}});
  }
  result(list);
}

void CameraPlugin::create(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<flutter::EncodableMap> reply)> result) {
  spdlog::debug("[camera_plugin] create:");

  // method arguments
  std::string cameraName;
  std::string resolutionPreset;
  int64_t fps = 0;
  int64_t videoBitrate = 0;
  int64_t audioBitrate = 0;
  bool enableAudio{};

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == kKeyCameraName && !it.second.IsNull() &&
        std::holds_alternative<std::string>(it.second)) {
      cameraName = std::get<std::string>(it.second);
    } else if (key == kKeyResolutionPreset && !it.second.IsNull()) {
      resolutionPreset = std::get<std::string>(it.second);
    } else if (key == kKeyFps && !it.second.IsNull() &&
               std::holds_alternative<int64_t>(it.second)) {
      fps = std::get<int64_t>(it.second);
    } else if (key == kKeyVideoBitrate && !it.second.IsNull() &&
               std::holds_alternative<int64_t>(it.second)) {
      videoBitrate = std::get<int64_t>(it.second);
    } else if (key == kKeyAudioBitrate && !it.second.IsNull() &&
               std::holds_alternative<int64_t>(it.second)) {
      audioBitrate = std::get<int64_t>(it.second);
    } else if (key == kKeyEnableAudio && !it.second.IsNull() &&
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
      {{flutter::EncodableValue(kKeyCameraId),
        flutter::EncodableValue(static_cast<int64_t>(g_cameras.size()))}}));
}

std::string CameraPlugin::RegisterEventChannel(
    const std::string& prefix,
    const std::string& uid,
    std::unique_ptr<flutter::StreamHandler<flutter::EncodableValue>> handler) {
  std::string channelName = prefix + uid;
  event_channels_[channelName] =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          messenger_, channelName,
          &flutter::StandardMethodCodec::GetInstance());
  stream_handlers_[channelName] = std::move(handler);

  event_channels_[channelName]->SetStreamHandler(
      std::move(stream_handlers_[channelName]));

  return uid;
}

class CameraStreamHandler
    : public flutter::StreamHandler<flutter::EncodableValue> {
 public:
  explicit CameraStreamHandler(CameraContext* ctx) { ctx_ = ctx; }

  std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
  OnListenInternal(
      const flutter::EncodableValue* /* arguments */,
      std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events)
      override {
    events_ = std::move(events);
    // We do this to bind the event to the main channel
    auto boundSendEvent = [this] {
      events_->Success(flutter::EncodableValue());
    };
    this->SetSendEventFunction(boundSendEvent);
    return nullptr;
  }

  std::unique_ptr<flutter::StreamHandlerError<flutter::EncodableValue>>
  OnCancelInternal(const flutter::EncodableValue* /* arguments */) override {
    events_->EndOfStream();
    return nullptr;
  }

  void SetSendEventFunction(std::function<void()> func) {
    sendEventFunc_ = func;
  }

 private:
  CameraContext* ctx_;
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> events_;
  std::function<void()> sendEventFunc_;
};

void CameraPlugin::initialize(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<std::string> reply)> result) {
  spdlog::debug("[camera_plugin] initialize:");
  spdlog::debug("\tPathForPicture: {}", GetFilePathForPicture().value());
  spdlog::debug("\tPathForVideo: {}", GetFilePathForVideo().value());

  // method arguments
  int32_t cameraId = 0;
  std::string imageFormatGroup;

  for (auto& it : args) {
    auto key = std::get<std::string>(it.first);
    if (key == kKeyCameraId && !it.second.IsNull() &&
        std::holds_alternative<int32_t>(it.second)) {
      cameraId = std::get<int32_t>(it.second);
    } else if (key == kKeyImageFormatGroup && !it.second.IsNull() &&
               std::holds_alternative<std::string>(it.second)) {
      imageFormatGroup = std::get<std::string>(it.second);
    } else {
      plugin_common::Encodable::PrintFlutterEncodableValue("unknown key",
                                                           it.first);
      plugin_common::Encodable::PrintFlutterEncodableValue("unknown value",
                                                           it.second);
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

    auto handler = std::make_unique<CameraStreamHandler>(camera.get());
    std::string channelName =
        RegisterEventChannel("flutter.io/cameraPlugin/camera",
                             std::to_string(cameraId), std::move(handler));

    camera->setCamera(g_camera_manager->get(id));
    camera->Initialize(texture_registrar_, messenger_, cameraId,
                       imageFormatGroup);
    result(channelName);
  }
}

void CameraPlugin::takePicture(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<std::string> reply)> /* result */) {
  plugin_common::Encodable::PrintFlutterEncodableMap("takePicture", args);
}

void CameraPlugin::startVideoRecording(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<std::string> reply)> /* result */) {
  plugin_common::Encodable::PrintFlutterEncodableMap("startVideoRecording",
                                                     args);
}

void CameraPlugin::stopVideoRecording(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<std::string> reply)> /* result */) {
  plugin_common::Encodable::PrintFlutterEncodableMap("stopVideoRecording",
                                                     args);
}

void CameraPlugin::pausePreview(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<std::string> reply)> /* result */) {
  plugin_common::Encodable::PrintFlutterEncodableMap("pausePreview", args);
}

void CameraPlugin::resumePreview(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<std::string>)> /* result */) {
  plugin_common::Encodable::PrintFlutterEncodableMap("resumePreview", args);
}

void CameraPlugin::dispose(
    const flutter::EncodableMap& args,
    std::function<void(ErrorOr<std::string> reply)> /* result */) {
  plugin_common::Encodable::PrintFlutterEncodableMap("dispose", args);
}

std::optional<std::string> CameraPlugin::GetFilePathForPicture() {
  std::ostringstream oss;
  oss << "xdg-user-dir PICTURES";
  char result[PATH_MAX];
  if (!Command::Execute(oss.str().c_str(), result)) {
    return std::nullopt;
  }
  std::string picture_path = result;
  std::filesystem::path path(StringTools::trim(picture_path, "\n"));
  path /= "PhotoCapture_" + TimeTools::GetCurrentTimeString() + "." +
          kPictureCaptureExtension;
  return path;
}

std::optional<std::string> CameraPlugin::GetFilePathForVideo() {
  std::ostringstream oss;
  oss << "xdg-user-dir VIDEOS";
  char result[PATH_MAX];
  if (!Command::Execute(oss.str().c_str(), result)) {
    return std::nullopt;
  }
  std::string video_path = result;
  std::filesystem::path path(StringTools::trim(video_path, "\n"));
  path /= "VideoCapture_" + TimeTools::GetCurrentTimeString() + "." +
          kVideoCaptureExtension;
  return path;
}

}  // namespace camera_plugin