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

static constexpr char kMethodAvailableCameras[] = "availableCameras";
static constexpr char kMethodCreate[] = "create";
static constexpr char kMethodInitialize[] = "initialize";
static constexpr char kMethodTakePicture[] = "takePicture";
static constexpr char kMethodStartVideoRecording[] = "startVideoRecording";
static constexpr char kMethodStopVideoRecording[] = "stopVideoRecording";
static constexpr char kMethodPausePreview[] = "pausePreview";
static constexpr char kMethodResumePreview[] = "resumePreview";
static constexpr char kMethodDispose[] = "dispose";

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
  auto channel = std::make_unique<flutter::MethodChannel<EncodableValue>>(
      binary_messenger, "plugins.flutter.io/camera", &GetCodec());
  if (api != nullptr) {
    channel->SetMethodCallHandler(
        [api](const MethodCall<EncodableValue>& methodCall,
              std::unique_ptr<MethodResult<EncodableValue>> result) {
          if (methodCall.method_name() == kMethodAvailableCameras) {
            try {
              api->availableCameras([reply = result.get()](
                                        ErrorOr<EncodableList>&& output) {
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
          } else if (methodCall.method_name() == kMethodCreate) {
            const auto& args =
                std::get_if<EncodableMap>(methodCall.arguments());
            api->create(*args, [reply = result.get()](
                                   ErrorOr<EncodableMap>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success(EncodableValue(std::move(output).TakeValue()));
            });
          } else if (methodCall.method_name() == kMethodInitialize) {
            const auto& args =
                std::get_if<EncodableMap>(methodCall.arguments());
            api->initialize(*args, [reply = result.get()](
                                       ErrorOr<std::string>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success();
            });
          } else if (methodCall.method_name() == kMethodTakePicture) {
            const auto& args =
                std::get_if<EncodableMap>(methodCall.arguments());
            api->takePicture(*args, [reply = result.get()](
                                        ErrorOr<std::string>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success();
            });
          } else if (methodCall.method_name() == kMethodStartVideoRecording) {
            const auto& args =
                std::get_if<EncodableMap>(methodCall.arguments());
            api->startVideoRecording(*args, [reply = result.get()](
                                                ErrorOr<std::string>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success();
            });
          } else if (methodCall.method_name() == kMethodStopVideoRecording) {
            const auto& args =
                std::get_if<EncodableMap>(methodCall.arguments());
            api->stopVideoRecording(*args, [reply = result.get()](
                                               ErrorOr<std::string>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success();
            });
          } else if (methodCall.method_name() == kMethodPausePreview) {
            const auto& args =
                std::get_if<EncodableMap>(methodCall.arguments());
            api->pausePreview(*args, [reply = result.get()](
                                         ErrorOr<std::string>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success();
            });
          } else if (methodCall.method_name() == kMethodResumePreview) {
            const auto& args =
                std::get_if<EncodableMap>(methodCall.arguments());
            api->resumePreview(*args, [reply = result.get()](
                                          ErrorOr<std::string>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success();
            });
          } else if (methodCall.method_name() == kMethodDispose) {
            const auto& args =
                std::get_if<EncodableMap>(methodCall.arguments());
            api->dispose(*args, [reply = result.get()](
                                    ErrorOr<std::string>&& output) {
              if (output.has_error()) {
                reply->Error(output.error().code(), output.error().message(),
                             output.error().details());
                return;
              }
              reply->Success();
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