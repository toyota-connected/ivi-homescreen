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

#include "messages.g.h"

#include <map>
#include <optional>
#include <sstream>
#include <string>

#include <flutter/basic_message_channel.h>
#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include "plugins/common/common.h"

using flutter::EncodableValue;
using flutter::MessageReply;
using flutter::MethodCall;
using flutter::MethodResult;

namespace plugin_filament_view {

void FilamentViewApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                            FilamentViewApi* api,
                            int32_t id) {
  {
    std::stringstream ss;
    ss << "io.sourcya.playx.3d.scene.channel_" << id;
    const auto channel =
        std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
            binary_messenger, ss.str().c_str(),
            &flutter::StandardMethodCodec::GetInstance());
    if (api != nullptr) {
      channel->SetMethodCallHandler(
          [api](const MethodCall<EncodableValue>& methodCall,
                std::unique_ptr<MethodResult<EncodableValue>> result) {
            SPDLOG_DEBUG("[{}]", methodCall.method_name());
            if (methodCall.method_name() == "CHANGE_ANIMATION_BY_INDEX") {
              result->Success();
            } else {
              result->NotImplemented();
            }
#if 0
                            try {
                              const auto& args = std::get<EncodableList>(message);
                              const auto& encodable_app_arg = args.at(0);
                              if (encodable_app_arg.IsNull()) {
                                reply(WrapError("app_arg unexpectedly null."));
                                return;
                              }
                              const auto& app_arg =
                                  std::any_cast<const FirestorePigeonFirebaseApp&>(
                                      std::get<CustomEncodableValue>(encodable_app_arg));
                              const auto& encodable_bundle_arg = args.at(1);
                              if (encodable_bundle_arg.IsNull()) {
                                reply(WrapError("bundle_arg unexpectedly null."));
                                return;
                              }
                              const auto& bundle_arg =
                                  std::get<std::vector<uint8_t>>(encodable_bundle_arg);
                              api->LoadBundle(
                                  app_arg, bundle_arg, [reply](ErrorOr<std::string>&& output) {
                                    if (output.has_error()) {
                                      reply(WrapError(output.error()));
                                      return;
                                    }
                                    EncodableList wrapped;
                                    wrapped.emplace_back(std::move(output).TakeValue());
                                    reply(EncodableValue(std::move(wrapped)));
                                  });
                            } catch (const std::exception& exception) {
                              reply(WrapError(exception.what()));
                            }
#endif
          });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

void ModelStateChannelApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                                 FilamentViewApi* api,
                                 int32_t id) {
  {
    std::stringstream ss;
    ss << "io.sourcya.playx.3d.scene.model_state_channel_" << id;
    const auto channel =
        std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
            binary_messenger, ss.str().c_str(),
            &flutter::StandardMethodCodec::GetInstance());
    if (api != nullptr) {
      channel->SetMethodCallHandler(
          [api](const MethodCall<EncodableValue>& methodCall,
                std::unique_ptr<MethodResult<EncodableValue>> result) {
            if (methodCall.method_name() == "listen") {
              result->Success();
            } else {
              spdlog::error("[{}]", methodCall.method_name());
              result->NotImplemented();
            }
          });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

void SceneStateApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                          FilamentViewApi* api,
                          int32_t id) {
  {
    std::stringstream ss;
    ss << "io.sourcya.playx.3d.scene.scene_state_" << id;
    const auto channel =
        std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
            binary_messenger, ss.str().c_str(),
            &flutter::StandardMethodCodec::GetInstance());
    if (api != nullptr) {
      channel->SetMethodCallHandler(
          [api](const MethodCall<EncodableValue>& methodCall,
                std::unique_ptr<MethodResult<EncodableValue>> result) {
            if (methodCall.method_name() == "listen") {
              result->Success();
            } else {
              spdlog::error("[{}]", methodCall.method_name());
              result->NotImplemented();
            }
          });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

void ShapeStateApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                          FilamentViewApi* api,
                          int32_t id) {
  {
    std::stringstream ss;
    ss << "io.sourcya.playx.3d.scene.shape_state_" << id;
    const auto channel =
        std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
            binary_messenger, ss.str().c_str(),
            &flutter::StandardMethodCodec::GetInstance());
    if (api != nullptr) {
      channel->SetMethodCallHandler(
          [api](const MethodCall<EncodableValue>& methodCall,
                std::unique_ptr<MethodResult<EncodableValue>> result) {
            if (methodCall.method_name() == "listen") {
              result->Success();
            } else {
              spdlog::error("[{}]", methodCall.method_name());
              result->NotImplemented();
            }
          });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

void RendererChannelApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                               FilamentViewApi* api,
                               int32_t id) {
  {
    std::stringstream ss;
    ss << "io.sourcya.playx.3d.scene.renderer_channel_" << id;
    const auto channel =
        std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
            binary_messenger, ss.str().c_str(),
            &flutter::StandardMethodCodec::GetInstance());
    if (api != nullptr) {
      channel->SetMethodCallHandler(
          [api](const MethodCall<EncodableValue>& methodCall,
                std::unique_ptr<MethodResult<EncodableValue>> result) {
            if (methodCall.method_name() == "listen") {
              result->Success();
            } else {
              spdlog::error("[{}]", methodCall.method_name());
              result->NotImplemented();
            }
          });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

}  // namespace plugin_filament_view
