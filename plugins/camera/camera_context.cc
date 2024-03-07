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

#include <flutter/event_channel.h>
#include <flutter/event_stream_handler_functions.h>
#include <flutter/standard_message_codec.h>

#include <libcamera/control_ids.h>
#include <libcamera/property_ids.h>

#include <utility>

#include <plugins/common/common.h>

namespace camera_plugin {

static constexpr char kPictureCaptureExtension[] = "jpeg";
static constexpr char kVideoCaptureExtension[] = "mp4";

using namespace plugin_common;

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
  spdlog::debug("[camera_plugin]");
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
    spdlog::error("[camera_plugin] Failed to acquire camera: {}", res);
  }

  spdlog::debug("[camera_plugin] Controls:");
  for (const auto& [id, info] : mCamera->controls()) {
    spdlog::debug("\t[{}] {}", id->name(), info.toString());
  }

  spdlog::debug("[camera_plugin] Properties:");
  for (const auto& [key, value] : mCamera->properties()) {
    const auto* id = libcamera::properties::properties.at(key);
    spdlog::debug("\t[{}] {}", id->name(), value.toString());
  }
}

CameraContext::~CameraContext() {
  SPDLOG_DEBUG("[camera_plugin] ~CameraContext()");
  mCamera->release();
  mCameraState = CAM_STATE_AVAILABLE;
}

void CameraContext::setCamera(std::shared_ptr<libcamera::Camera> camera) {
  mCamera = std::move(camera);
}

std::string CameraContext::Initialize(
    flutter::PluginRegistrar* plugin_registrar,
    int64_t camera_id,
    const std::string& image_format_group) {
  if (mPreview.is_initialized) {
    return {};
  }

  std::string channel_name =
      std::string("flutter.io/cameraPlugin/camera") + std::to_string(camera_id);

  camera_channel_ = std::make_unique<flutter::MethodChannel<>>(
      plugin_registrar->messenger(), channel_name,
      &flutter::StandardMethodCodec::GetInstance());

  camera_id_ = camera_id;
  texture_registrar_ = plugin_registrar->texture_registrar();
  mImageFormatGroup.assign(image_format_group);

  spdlog::debug(
      "[camera_plugin] Initialize: cameraId: {}, imageFormatGroup: [{}]",
      camera_id, mImageFormatGroup);

  mPreview.width = 640;
  mPreview.height = 480;

  /// Setup GL Texture 2D
  texture_registrar_->TextureMakeCurrent();
  glGenFramebuffers(1, &mPreview.framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, mPreview.framebuffer);

  glGenTextures(1, &mPreview.textureId);
  glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  glBindTexture(GL_TEXTURE_2D, mPreview.textureId);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mPreview.width, mPreview.height, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glBindTexture(GL_TEXTURE_2D, 0);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                         mPreview.textureId, 0);

  auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    spdlog::error("[camera_plugin] FramebufferStatus: 0x{:X}", status);
  }

  glFinish();

  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  texture_registrar_->TextureClearCurrent();

  mPreview.descriptor = {
      .struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor),
      .handle = &mPreview.textureId,
      .width = static_cast<size_t>(mPreview.width),
      .height = static_cast<size_t>(mPreview.height),
      .visible_width = static_cast<size_t>(mPreview.width),
      .visible_height = static_cast<size_t>(mPreview.height),
      .format = kFlutterDesktopPixelFormatRGBA8888,
      .release_callback = [](void* /* release_context */) {},
      .release_context = this,
  };

  mPreview.gpu_surface_texture = std::make_unique<flutter::GpuSurfaceTexture>(
      FlutterDesktopGpuSurfaceType::kFlutterDesktopGpuSurfaceTypeGlTexture2D,
      [&](size_t /* width */,
          size_t /* height */) -> const FlutterDesktopGpuSurfaceDescriptor* {
        return &mPreview.descriptor;
      });

  flutter::TextureVariant texture = *mPreview.gpu_surface_texture;
  texture_registrar_->RegisterTexture(&texture);
  texture_registrar_->MarkTextureFrameAvailable(mPreview.textureId);

  auto props = mCamera->properties();

  std::string exposureMode("auto");
  bool exposurePointSupported{};
  std::string focusMode("locked");
  bool focusPointSupported{};

  camera_channel_->InvokeMethod(
      "initialized",
      std::make_unique<flutter::EncodableValue>(
          flutter::EncodableValue(flutter::EncodableMap(
              {{flutter::EncodableValue("cameraId"),
                flutter::EncodableValue(camera_id)},
               {flutter::EncodableValue("previewWidth"),
                flutter::EncodableValue(static_cast<double>(mPreview.width))},
               {flutter::EncodableValue("previewHeight"),
                flutter::EncodableValue(static_cast<double>(mPreview.height))},
               {flutter::EncodableValue("exposureMode"),
                flutter::EncodableValue(exposureMode.c_str())},
               {flutter::EncodableValue("exposurePointSupported"),
                flutter::EncodableValue(exposurePointSupported)},
               {flutter::EncodableValue("focusMode"),
                flutter::EncodableValue(focusMode.c_str())},
               {flutter::EncodableValue("focusPointSupported"),
                flutter::EncodableValue(focusPointSupported)}}))));

  mPreview.is_initialized = true;

  return channel_name;
}

std::optional<std::string> CameraContext::GetFilePathForPicture() {
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

std::optional<std::string> CameraContext::GetFilePathForVideo() {
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

std::string CameraContext::takePicture() {
  auto filename = GetFilePathForPicture();
  if (filename.has_value()) {
    return filename.value();
  }
  return {};
}

void CameraContext::startVideoRecording(bool /* enableStream */) {
  SPDLOG_DEBUG("[camera_plugin] startVideoRecording");
}

void CameraContext::pauseVideoRecording() {
  SPDLOG_DEBUG("[camera_plugin] pauseVideoRecording");
}

void CameraContext::resumeVideoRecording() {
  SPDLOG_DEBUG("[camera_plugin] resumeVideoRecording");
}

std::string CameraContext::stopVideoRecording() {
  auto filename = GetFilePathForVideo();
  if (filename.has_value()) {
    SPDLOG_DEBUG("[camera_plugin] stopVideoRecording: [{}]", filename.value());
    return filename.value();
  }
  SPDLOG_DEBUG("[camera_plugin] stopVideoRecording: []");
  return {};
}

}  // namespace camera_plugin