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

#include <flutter/basic_message_channel.h>
#include <flutter/event_channel.h>
#include <shell/platform/embedder/embedder.h>

#include <libcamera/libcamera.h>

#include "engine.h"

namespace camera_plugin {

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

  std::string Initialize(flutter::PluginRegistrar* plugin_registrar,
                         int64_t camera_id,
                         const std::string& image_format_group);

  const std::string& getCameraId() { return mCamera->id(); }

  CAM_STATE_T getCameraState() { return mCameraState; }

  static std::optional<std::string> GetFilePathForPicture();

  static std::optional<std::string> GetFilePathForVideo();

  static std::string takePicture();

  void startVideoRecording(bool enableStream);
  void pauseVideoRecording();
  void resumeVideoRecording();
  std::string stopVideoRecording();

 private:
  flutter::TextureRegistrar* texture_registrar_{};
  std::unique_ptr<flutter::MethodChannel<>> camera_channel_;
  int64_t camera_id_ = -1;
  CAM_STATE_T mCameraState;

  // The internal Flutter event channel instance.
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>>
      event_channel_;

  // The internal Flutter event sink instance, used to send events to the Dart
  // side.
  std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink_;

  std::string mCameraName;
  std::string mResolutionPreset;
  int64_t mFps;
  int64_t mVideoBitrate;
  int64_t mAudioBitrate;
  bool mEnableAudio;
  std::string mImageFormatGroup;
  std::shared_ptr<libcamera::Camera> mCamera{};

  struct preview {
    bool is_initialized{};

    // The internal Flutter event channel instance.
    flutter::EventChannel<flutter::EncodableValue>* event_channel_;

    // The internal Flutter event sink instance, used to send events to the Dart
    // side.
    std::unique_ptr<flutter::EventSink<flutter::EncodableValue>> event_sink;

    GLuint textureId{};
    GLuint framebuffer{};
    GLuint program;
    GLsizei width, height;
    GLuint vertex_arr_id_{};

    // The Surface Descriptor sent to Flutter when a texture frame is available.
    std::unique_ptr<flutter::GpuSurfaceTexture> gpu_surface_texture;
    FlutterDesktopGpuSurfaceDescriptor descriptor{};
  } mPreview;

  flutter::MethodChannel<>* GetMethodChannel();
};
}  // namespace camera_plugin
#endif  // FLUTTER_PLUGIN_CAMERA_CONTEXT_H_
