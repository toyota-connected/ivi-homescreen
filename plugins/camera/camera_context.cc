/*
 * Copyright 2023-2024 Toyota Connected North America
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

#include "camera_context.h"
#include "event_channel.h"
#include "standard_message_codec.h"

#include <libcamera/control_ids.h>
#include <libcamera/property_ids.h>

#include <flutter/shell/platform/common/json_method_codec.h>

#include <utility>

CameraContext::CameraContext(std::string cameraName,
                             std::string resolutionPreset,
                             int64_t fps,
                             int64_t videoBitrate,
                             int64_t audioBitrate,
                             bool enableAudio,
                             std::shared_ptr<libcamera::Camera> camera)
    : mCameraName(std::move(cameraName)),
      mResolutionPreset(std::move(resolutionPreset)),
      mFps(fps),
      mVideoBitrate(videoBitrate),
      mAudioBitrate(audioBitrate),
      mEnableAudio(enableAudio),
      mCamera(std::move(camera)) {
  spdlog::debug("\tcameraName: [{}]", mCameraName);
  spdlog::debug("\tresolutionPreset: [{}]", mResolutionPreset);
  spdlog::debug("\tfps: [{}]", mFps);
  spdlog::debug("\tvideoBitrate: [{}]", mVideoBitrate);
  spdlog::debug("\taudioBitrate: [{}]", mAudioBitrate);
  spdlog::debug("\tenableAudio: [{}]", mEnableAudio);
  mCameraState = CAM_STATE_AVAILABLE;
  auto res = mCamera->acquire();
  if (res == 0) {
    if (mCameraState == CAM_STATE_AVAILABLE) {
      mCameraState = CAM_STATE_ACQUIRED;
    }
  } else {
    spdlog::error("Failed to acquire camera: {}", res);
  }

  spdlog::debug("Controls:");
  for (const auto& [id, info] : mCamera->controls()) {
    spdlog::debug("\t[{}] {}", id->name(), info.toString());
  }

  spdlog::debug("Properties:");
  for (const auto& [key, value] : mCamera->properties()) {
    const auto* id = libcamera::properties::properties.at(key);
    spdlog::debug("\t[{}] {}", id->name(), value.toString());
  }
}

CameraContext::~CameraContext() {
  SPDLOG_DEBUG("~CameraContext()");
  mCamera->release();
  mCameraState = CAM_STATE_AVAILABLE;
}

void CameraContext::setCamera(std::shared_ptr<libcamera::Camera> camera) {
  mCamera = std::move(camera);
}

void CameraContext::Initialize(flutter::TextureRegistrar* texture_registrar,
                               flutter::BinaryMessenger* messenger,
                               int64_t camera_id,
                               const std::string& image_format_group) {
  messenger_ = messenger;
  texture_registrar_ = texture_registrar;
  mImageFormatGroup.assign(image_format_group);

  spdlog::debug("\tcameraId: {}", camera_id);
  spdlog::debug("\timageFormatGroup: [{}]", mImageFormatGroup);

  auto props = mCamera->properties();

  double previewWidth = 640;
  double previewHeight = 480;
  std::string exposureMode("auto");
  bool exposurePointSupported{};
  std::string focusMode("locked");
  bool focusPointSupported{};

  flutter::EncodableMap map = {
      {flutter::EncodableValue("cameraId"), flutter::EncodableValue(camera_id)},
      {flutter::EncodableValue("previewWidth"),
       flutter::EncodableValue(previewWidth)},
      {flutter::EncodableValue("previewHeight"),
       flutter::EncodableValue(previewHeight)},
      {flutter::EncodableValue("exposureMode"),
       flutter::EncodableValue(exposureMode.c_str())},
      {flutter::EncodableValue("exposurePointSupported"),
       flutter::EncodableValue(exposurePointSupported)},
      {flutter::EncodableValue("focusMode"),
       flutter::EncodableValue(focusMode.c_str())},
      {flutter::EncodableValue("focusPointSupported"),
       flutter::EncodableValue(focusPointSupported)},
  };

  auto& codec = flutter::StandardMessageCodec::GetInstance();
  std::unique_ptr<std::vector<uint8_t>> result =
      codec.EncodeMessage(flutter::EncodableValue(std::move(map)));
  auto channel = "flutter.io/cameraPlugin/camera" + std::to_string(camera_id);
  messenger_->Send(channel, result->data(), result->size());
}