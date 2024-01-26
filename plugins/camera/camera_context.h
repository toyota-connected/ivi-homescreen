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
#ifndef FLUTTER_PLUGIN_CAMERA_CONTEXT_H_
#define FLUTTER_PLUGIN_CAMERA_CONTEXT_H_

#include <shell/platform/embedder/embedder.h>

#include <libcamera/libcamera.h>

#include "engine.h"

class CameraContext : public flutter::Plugin {
 public:
  typedef enum {
    CAM_STATE_AVAILABLE,
    CAM_STATE_ACQUIRED,
    CAM_STATE_CONFIGURED,
    CAM_STATE_RUNNING,
    CAM_STATE_STOPPING,
  } CAM_STATE_T;

  CameraContext(std::string cameraName,
                std::string resolutionPreset,
                int64_t fps,
                int64_t videoBitrate,
                int64_t audioBitrate,
                bool enableAudio,
                std::shared_ptr<libcamera::Camera> camera);
  ~CameraContext() override;

  void setCamera(std::shared_ptr<libcamera::Camera> camera);

  void Initialize(flutter::TextureRegistrar* texture_registrar,
                  flutter::BinaryMessenger* messenger,
                  int64_t camera_id,
                  const std::string& image_format_group);

  const std::string& getCameraId() { return mCamera->id(); }

  CAM_STATE_T getCameraState() { return mCameraState; }

 private:
  flutter::BinaryMessenger* messenger_{};
  flutter::TextureRegistrar* texture_registrar_{};
  CAM_STATE_T mCameraState;
  static constexpr char kEventPrefix[] = "flutter.io/cameraPlugin/camera";

  std::string mCameraName;
  std::string mResolutionPreset;
  int64_t mFps;
  int64_t mVideoBitrate;
  int64_t mAudioBitrate;
  bool mEnableAudio;
  std::string mImageFormatGroup;
  std::shared_ptr<libcamera::Camera> mCamera{};
};

#endif  // FLUTTER_PLUGIN_CAMERA_CONTEXT_H_