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

#ifndef FLUTTER_PLUGIN_CAMERA_PLUGIN_H_
#define FLUTTER_PLUGIN_CAMERA_PLUGIN_H_

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>

#include <libcamera/camera.h>

#include "camera_context.h"

#include "event_channel.h"
#include "messages.h"
#include "plugins/common/common.h"

namespace camera_plugin {

class CameraPlugin final : public flutter::Plugin, public CameraApi {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar);

  CameraPlugin(flutter::TextureRegistrar* texture_registrar,
               flutter::BinaryMessenger* messenger);

  ~CameraPlugin() override;

  void availableCameras(
      std::function<void(ErrorOr<flutter::EncodableList> reply)> result)
      override;
  void create(const flutter::EncodableMap& args,
              std::function<void(ErrorOr<flutter::EncodableMap> reply)> result)
      override;
  void initialize(
      const flutter::EncodableMap& args,
      std::function<void(ErrorOr<std::string> reply)> result) override;
  void takePicture(
      const flutter::EncodableMap& args,
      std::function<void(ErrorOr<std::string> reply)> result) override;
  void startVideoRecording(
      const flutter::EncodableMap& args,
      std::function<void(ErrorOr<std::string> reply)> result) override;
  void stopVideoRecording(
      const flutter::EncodableMap& args,
      std::function<void(ErrorOr<std::string> reply)> result) override;
  void pausePreview(
      const flutter::EncodableMap& args,
      std::function<void(ErrorOr<std::string> reply)> result) override;
  void resumePreview(
      const flutter::EncodableMap& args,
      std::function<void(ErrorOr<std::string> reply)> result) override;
  void dispose(const flutter::EncodableMap& args,
               std::function<void(ErrorOr<std::string> reply)> result) override;

  // Disallow copy and assign.
  CameraPlugin(const CameraPlugin&) = delete;
  CameraPlugin& operator=(const CameraPlugin&) = delete;

 private:
  flutter::TextureRegistrar* texture_registrar_;
  flutter::BinaryMessenger* messenger_;
  std::map<std::string,
           std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>>
      event_channels_;
  std::map<std::string, std::unique_ptr<flutter::StreamHandler<>>>
      stream_handlers_;

  static void camera_added(const std::shared_ptr<libcamera::Camera>& cam);
  static void camera_removed(const std::shared_ptr<libcamera::Camera>& cam);

  static std::string get_camera_lens_facing(
      const std::shared_ptr<libcamera::Camera>& camera);

  static std::optional<std::string> GetFilePathForPicture();
  static std::optional<std::string> GetFilePathForVideo();

  std::string RegisterEventChannel(
      const std::string& prefix,
      const std::string& uid,
      std::unique_ptr<flutter::StreamHandler<flutter::EncodableValue>> handler);
};
}  // namespace camera_plugin

#endif  // FLUTTER_PLUGIN_CAMERA_PLUGIN_H_