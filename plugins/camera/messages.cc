/*
 * Copyright 2023 Toyota Connected North America
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

#undef _HAS_EXCEPTIONS

#include "messages.h"

#include <flutter/basic_message_channel.h>
#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/method_call.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include <optional>

#include "camera_plugin.h"

namespace camera_plugin {

using flutter::BasicMessageChannel;
using flutter::CustomEncodableValue;
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;
using flutter::MethodCall;
using flutter::MethodResult;

/// The codec used by CameraApi.
const flutter::StandardMethodCodec& CameraApi::GetCodec() {
  return flutter::StandardMethodCodec::GetInstance();
}

// Sets up an instance of `CameraApi` to handle messages through the
// `binary_messenger`.
void CameraApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                      CameraApi* api) {
  auto channel = std::make_unique<flutter::MethodChannel<>>(
      binary_messenger, "plugins.flutter.io/camera", &GetCodec());
  if (api != nullptr) {
    channel->SetMethodCallHandler([api](const MethodCall<EncodableValue>&
                                            methodCall,
                                        std::unique_ptr<MethodResult<
                                            EncodableValue>> result) {
      if (methodCall.method_name() == "availableCameras") {
        try {
          api->availableCameras(
              [reply = result.get()](ErrorOr<EncodableList>&& output) {
                if (output.has_error()) {
                  reply->Error(output.error().code(), output.error().message(),
                               output.error().details());
                  return;
                }
                reply->Success(EncodableValue(std::move(output).TakeValue()));
              });
        } catch (const std::exception& exception) {
          result->Error(exception.what());
        }
      } else if (methodCall.method_name() == "create") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->create(
            *args, [reply = result.get()](ErrorOr<EncodableMap>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "initialize") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->initialize(
            *args, [reply = result.get()](ErrorOr<std::string>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "takePicture") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->takePicture(
            *args, [reply = result.get()](ErrorOr<std::string>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "startVideoRecording") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->startVideoRecording(
            *args,
            [reply = result.get()](std::optional<FlutterError>&& output) {
              if (output.has_value()) {
                reply->Error(output.value().code(), output.value().message(),
                             output.value().details());
                return;
              }
              reply->Success();
            });
      } else if (methodCall.method_name() == "pauseVideoRecording") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->pauseVideoRecording(
            *args,
            [reply = result.get()](std::optional<FlutterError>&& output) {
              if (output.has_value()) {
                reply->Error(output.value().code(), output.value().message(),
                             output.value().details());
                return;
              }
              reply->Success();
            });
      } else if (methodCall.method_name() == "resumeVideoRecording") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->resumeVideoRecording(
            *args,
            [reply = result.get()](std::optional<FlutterError>&& output) {
              if (output.has_value()) {
                reply->Error(output.value().code(), output.value().message(),
                             output.value().details());
                return;
              }
              reply->Success();
            });
      } else if (methodCall.method_name() == "stopVideoRecording") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->stopVideoRecording(
            *args, [reply = result.get()](ErrorOr<std::string>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "pausePreview") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->pausePreview(
            *args, [reply = result.get()](ErrorOr<double>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "resumePreview") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->resumePreview(
            *args, [reply = result.get()](ErrorOr<double>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "lockCaptureOrientation") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->lockCaptureOrientation(
            *args, [reply = result.get()](ErrorOr<std::string>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "unlockCaptureOrientation") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->unlockCaptureOrientation(
            *args, [reply = result.get()](ErrorOr<std::string>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "setFlashMode") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->setFlashMode(*args, [reply = result.get()](
                                     std::optional<FlutterError>&& output) {
          if (output.has_value()) {
            reply->Error(output.value().code(), output.value().message(),
                         output.value().details());
            return;
          }
          reply->Success();
        });
      } else if (methodCall.method_name() == "setFocusMode") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->setFocusMode(*args, [reply = result.get()](
                                     std::optional<FlutterError>&& output) {
          if (output.has_value()) {
            reply->Error(output.value().code(), output.value().message(),
                         output.value().details());
            return;
          }
          reply->Success();
        });
      } else if (methodCall.method_name() == "setExposureMode") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->setExposureMode(*args, [reply = result.get()](
                                        std::optional<FlutterError>&& output) {
          if (output.has_value()) {
            reply->Error(output.value().code(), output.value().message(),
                         output.value().details());
            return;
          }
          reply->Success();
        });
      } else if (methodCall.method_name() == "getExposureOffsetStepSize") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->getExposureOffsetStepSize(
            *args, [reply = result.get()](ErrorOr<double>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "dispose") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->dispose(*args, [reply = result.get()](
                                std::optional<FlutterError>&& output) {
          if (output.has_value()) {
            reply->Error(output.value().code(), output.value().message(),
                         output.value().details());
            return;
          }
          reply->Success();
        });
      } else if (methodCall.method_name() == "setExposurePoint") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->setExposurePoint(*args, [reply = result.get()](
                                         std::optional<FlutterError>&& output) {
          if (output.has_value()) {
            reply->Error(output.value().code(), output.value().message(),
                         output.value().details());
            return;
          }
          reply->Success();
        });
      } else if (methodCall.method_name() == "setFocusPoint") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->setFocusPoint(*args, [reply = result.get()](
                                      std::optional<FlutterError>&& output) {
          if (output.has_value()) {
            reply->Error(output.value().code(), output.value().message(),
                         output.value().details());
            return;
          }
          reply->Success();
        });
      } else if (methodCall.method_name() == "setExposureOffset") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->setExposureOffset(
            *args, [reply = result.get()](ErrorOr<double>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "getMinExposureOffset") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->getMinExposureOffset(
            *args, [reply = result.get()](ErrorOr<double>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "getMaxExposureOffset") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->getMaxExposureOffset(
            *args, [reply = result.get()](ErrorOr<double>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "getMaxZoomLevel") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->getMaxZoomLevel(
            *args, [reply = result.get()](ErrorOr<double>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else if (methodCall.method_name() == "getMinZoomLevel") {
        const auto& args = std::get_if<EncodableMap>(methodCall.arguments());
        api->getMinZoomLevel(
            *args, [reply = result.get()](ErrorOr<double>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
      } else {
        result->NotImplemented();
      }
    });
  } else {
    channel->SetMethodCallHandler(nullptr);
  }
}

}  // namespace camera_plugin