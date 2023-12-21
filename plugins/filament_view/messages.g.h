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

#ifndef PIGEON_MESSAGES_G_H_
#define PIGEON_MESSAGES_G_H_

#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/standard_method_codec.h>

#include <map>
#include <optional>
#include <string>
#include <utility>

#include <flutter/standard_method_codec.h>

namespace plugin_filament_view {

class FlutterError {
 public:
  explicit FlutterError(std::string code) : code_(std::move(code)) {}

  explicit FlutterError(std::string code, std::string message)
      : code_(std::move(code)), message_(std::move(message)) {}

  explicit FlutterError(std::string code,
                        std::string message,
                        flutter::EncodableValue details)
      : code_(std::move(code)),
        message_(std::move(message)),
        details_(std::move(details)) {}

  const std::string& code() const { return code_; }

  const std::string& message() const { return message_; }

  const flutter::EncodableValue& details() const { return details_; }

 private:
  std::string code_;
  std::string message_;
  flutter::EncodableValue details_;
};

template <class T>
class ErrorOr {
 public:
  ErrorOr(const T& rhs) : v_(rhs) {}

  ErrorOr(const T&& rhs) : v_(std::move(rhs)) {}

  ErrorOr(const FlutterError& rhs) : v_(rhs) {}

  ErrorOr(const FlutterError&& rhs) : v_(rhs) {}

  bool has_error() const { return std::holds_alternative<FlutterError>(v_); }

  const T& value() const { return std::get<T>(v_); };

  const FlutterError& error() const { return std::get<FlutterError>(v_); };

 private:
  friend class FilamentViewApi;

  friend class ModelStateChannelApi;

  friend class SceneStateApi;

  friend class ShapeStateApi;

  friend class RendererChannelApi;

  ErrorOr() = default;

  T TakeValue() && { return std::get<T>(std::move(v_)); }

  std::variant<T, FlutterError> v_;
};

class FilamentViewApi {
 public:
  FilamentViewApi(const FilamentViewApi&) = delete;

  FilamentViewApi& operator=(const FilamentViewApi&) = delete;

  virtual ~FilamentViewApi() = default;

  virtual void ChangeAnimationByIndex(
      const int32_t index,
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void ChangeAnimationByName(
      std::string name,
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void GetAnimationNames(
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void GetAnimationCount(
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void GetCurrentAnimationIndex(
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void GetAnimationNameByIndex(
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void ChangeSkyboxByAsset(
      std::string path,
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void ChangeSkyboxByUrl(
      std::string url,
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void ChangeSkyboxByHdrAsset(
      std::string path,
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void ChangeSkyboxByHdrUrl(
      std::string url,
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void ChangeSkyboxColor(
      std::string color,
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void ChangeToTransparentSkybox(
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void ChangeLightByKtxAsset(
      std::string path,
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void ChangeLightByKtxUrl(
      std::string url,
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void ChangeLightByIndirectLight(
      std::string path,
      double intensity,
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void ChangeLightByHdrUrl(
      std::string path,
      double intensity,
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

  virtual void ChangeToDefaultIndirectLight(
      const std::function<void(std::optional<FlutterError> reply)> result) = 0;

#if 0
        kMethodChangeLight
        kMethodChangeToDefaultLight
        kMethodLoadGlbModelFromAssets
        kMethodLoadGlbModelFromUrl
        kMethodLoadGltfModelFromAssets
        kMethodChangeModelScale
        kMethodChangeModelPosition
        kMethodGetCurrentModelState
        kMethodUpdateCamera
        kMethodUpdateExposure
        kMethodUpdateProjection
        kMethodUpdateLensProjection
        kMethodUpdateCameraShift
        kMethodUpdateCameraScaling
        kMethodSetDefaultCamera
        kMethodLookAtDefaultPosition
        kMethodGetLookAt
        kMethodLookAtPosition
        kMethodCameraScroll
        kMethodCameraGrabBegin
        kMethodCameraGrabUpdate
        kMethodCameraGrabEnd
        kMethodCameraRayCast
        kMethodUpdateGround
        kMethodUpdateGroundMaterial
        kMethodAddShape
        kMethodRemoveShape
        kMethodUpdateShape
        kMethodGetCurrentCreatedShapesIds
#endif

  // The codec used by FilamentViewApi.
  static const flutter::StandardMethodCodec& GetCodec();

  // Sets up an instance of `FilamentViewApi` to handle messages
  // through the `binary_messenger`.
  static void SetUp(flutter::BinaryMessenger* binary_messenger,
                    FilamentViewApi* api,
                    int32_t id);

  static flutter::EncodableValue WrapError(std::string_view error_message);

  static flutter::EncodableValue WrapError(const FlutterError& error);

 protected:
  FilamentViewApi() = default;
};

class ModelStateChannelApi {
 public:
  ModelStateChannelApi(const ModelStateChannelApi&) = delete;

  ModelStateChannelApi& operator=(const ModelStateChannelApi&) = delete;

  virtual ~ModelStateChannelApi() = default;

  // TODO

  // The codec used by ModelStateChannelApi.
  static const flutter::StandardMethodCodec& GetCodec();

  // Sets up an instance of `ModelStateChannelApi` to handle messages
  // through the `binary_messenger`.
  static void SetUp(flutter::BinaryMessenger* binary_messenger,
                    FilamentViewApi* api,
                    int32_t id);

  static flutter::EncodableValue WrapError(std::string_view error_message);

  static flutter::EncodableValue WrapError(const FlutterError& error);

 protected:
  ModelStateChannelApi() = default;
};

class SceneStateApi {
 public:
  SceneStateApi(const SceneStateApi&) = delete;

  SceneStateApi& operator=(const SceneStateApi&) = delete;

  virtual ~SceneStateApi() = default;

  // TODO

  // The codec used by SceneStateApi.
  static const flutter::StandardMethodCodec& GetCodec();

  // Sets up an instance of `SceneStateApi` to handle messages
  // through the `binary_messenger`.
  static void SetUp(flutter::BinaryMessenger* binary_messenger,
                    FilamentViewApi* api,
                    int32_t id);

  static flutter::EncodableValue WrapError(std::string_view error_message);

  static flutter::EncodableValue WrapError(const FlutterError& error);

 protected:
  SceneStateApi() = default;
};

class ShapeStateApi {
 public:
  ShapeStateApi(const ShapeStateApi&) = delete;

  ShapeStateApi& operator=(const ShapeStateApi&) = delete;

  virtual ~ShapeStateApi() = default;

  // TODO

  // The codec used by ShapeStateApi.
  static const flutter::StandardMethodCodec& GetCodec();

  // Sets up an instance of `ShapeStateApi` to handle messages
  // through the `binary_messenger`.
  static void SetUp(flutter::BinaryMessenger* binary_messenger,
                    FilamentViewApi* api,
                    int32_t id);

  static flutter::EncodableValue WrapError(std::string_view error_message);

  static flutter::EncodableValue WrapError(const FlutterError& error);

 protected:
  ShapeStateApi() = default;
};

class RendererChannelApi {
 public:
  RendererChannelApi(const RendererChannelApi&) = delete;

  RendererChannelApi& operator=(const RendererChannelApi&) = delete;

  virtual ~RendererChannelApi() = default;

  // TODO

  // The codec used by RendererChannelApi.
  static const flutter::StandardMethodCodec& GetCodec();

  // Sets up an instance of `RendererChannelApi` to handle messages
  // through the `binary_messenger`.
  static void SetUp(flutter::BinaryMessenger* binary_messenger,
                    FilamentViewApi* api,
                    int32_t id);

  static flutter::EncodableValue WrapError(std::string_view error_message);

  static flutter::EncodableValue WrapError(const FlutterError& error);

 protected:
  RendererChannelApi() = default;
};

}  // namespace plugin_filament_view

#endif  // PIGEON_MESSAGES_G_H_