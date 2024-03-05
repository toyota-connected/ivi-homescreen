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
#include "plugins/common/tools/encodable.h"

#include <flutter/basic_message_channel.h>
#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/method_call.h>
#include <flutter/method_channel.h>
#include <flutter/standard_method_codec.h>

#include <optional>
#include <string>

namespace desktop_window_linux_plugin {

using flutter::BasicMessageChannel;
using flutter::CustomEncodableValue;
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;
using flutter::MethodCall;
using flutter::MethodResult;

/// The codec used by DesktopWindowLinuxApi.
const flutter::StandardMethodCodec& DesktopWindowLinuxApi::GetCodec() {
  return flutter::StandardMethodCodec::GetInstance();
}

// Sets up an instance of `DesktopWindowLinuxApi` to handle messages through the
// `binary_messenger`.
void DesktopWindowLinuxApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                                  DesktopWindowLinuxApi* api) {
  {
    auto channel = std::make_unique<flutter::MethodChannel<EncodableValue>>(
        binary_messenger, "desktop_window", &GetCodec());
    if (api != nullptr) {
      channel->SetMethodCallHandler(
          [api](const flutter::MethodCall<EncodableValue>& call,
                const std::unique_ptr<flutter::MethodResult<EncodableValue>>&
                    result) {
            if (call.method_name() == "getWindowSize") {
              double width = 0;
              double height = 0;
              api->getWindowSize(width, height);
              result->Success(EncodableValue(EncodableList{
                  EncodableValue(width), EncodableValue(height)}));
            } else if (call.method_name() == "setWindowSize") {
              const auto& args = std::get_if<EncodableMap>(call.arguments());
              double width = 0;
              double height = 0;
              for (auto& it : *args) {
                if ("width" == std::get<std::string>(it.first) &&
                    std::holds_alternative<double>(it.second)) {
                  width = std::get<double>(it.second);
                } else if ("height" == std::get<std::string>(it.first) &&
                           std::holds_alternative<double>(it.second)) {
                  height = std::get<double>(it.second);
                }
              }
              if (width == 0 || height == 0) {
                result->Error("argument_error", "width or height not provided");
                return std::nullopt;
              }
              api->setWindowSize(width, height);
              result->Success(EncodableValue(true));
            } else if (call.method_name() == "setMinWindowSize") {
              const auto& args = std::get_if<EncodableMap>(call.arguments());
              double width = 0;
              double height = 0;
              for (auto& it : *args) {
                if ("width" == std::get<std::string>(it.first) &&
                    std::holds_alternative<double>(it.second)) {
                  width = std::get<double>(it.second);
                } else if ("height" == std::get<std::string>(it.first) &&
                           std::holds_alternative<double>(it.second)) {
                  height = std::get<double>(it.second);
                }
              }
              if (width == 0 || height == 0) {
                result->Error("argument_error", "width or height not provided");
                return std::nullopt;
              }
              api->setMinWindowSize(width, height);
              result->Success(EncodableValue(true));
            } else if (call.method_name() == "setMaxWindowSize") {
              const auto& args = std::get_if<EncodableMap>(call.arguments());
              double width = 0;
              double height = 0;
              for (auto& it : *args) {
                if ("width" == std::get<std::string>(it.first) &&
                    std::holds_alternative<double>(it.second)) {
                  width = std::get<double>(it.second);
                } else if ("height" == std::get<std::string>(it.first) &&
                           std::holds_alternative<double>(it.second)) {
                  height = std::get<double>(it.second);
                }
              }
              if (width == 0 || height == 0) {
                result->Error("argument_error", "width or height not provided");
                return std::nullopt;
              }
              api->setMaxWindowSize(width, height);
              result->Success(EncodableValue(true));
            } else if (call.method_name() == "resetMaxWindowSize") {
              const auto& args = std::get_if<EncodableMap>(call.arguments());
              double width = 0;
              double height = 0;
              for (auto& it : *args) {
                if ("width" == std::get<std::string>(it.first) &&
                    std::holds_alternative<double>(it.second)) {
                  width = std::get<double>(it.second);
                } else if ("height" == std::get<std::string>(it.first) &&
                           std::holds_alternative<double>(it.second)) {
                  height = std::get<double>(it.second);
                }
              }
              if (width == 0 || height == 0) {
                result->Error("argument_error", "width or height not provided");
                return std::nullopt;
              }
              api->resetMaxWindowSize(width, height);
              result->Success(EncodableValue(true));
            } else if (call.method_name() == "toggleFullScreen") {
              plugin_common::Encodable::PrintFlutterEncodableValue(
                  "toggleFullScreen", *call.arguments());
              api->toggleFullScreen();
              result->Success(EncodableValue(true));
            } else if (call.method_name() == "setFullScreen") {
              const auto& args = std::get_if<EncodableMap>(call.arguments());
              bool fullscreen{};
              for (auto& it : *args) {
                if ("fullscreen" == std::get<std::string>(it.first) &&
                    std::holds_alternative<bool>(it.second)) {
                  fullscreen = std::get<bool>(it.second);
                }
              }
              api->setFullScreen(fullscreen);
              result->Success(EncodableValue(true));
            } else if (call.method_name() == "getFullScreen") {
              result->Success(EncodableValue(api->getFullScreen()));
            } else if (call.method_name() == "hasBorders") {
              result->Success(EncodableValue(api->hasBorders()));
            } else if (call.method_name() == "setBorders") {
              const auto& args = std::get_if<EncodableMap>(call.arguments());
              bool border{};
              for (auto& it : *args) {
                if ("border" == std::get<std::string>(it.first) &&
                    std::holds_alternative<bool>(it.second)) {
                  border = std::get<bool>(it.second);
                }
              }
              api->setBorders(border);
              result->Success(EncodableValue(true));
            } else if (call.method_name() == "toggleBorders") {
              api->toggleBorders();
              result->Success(EncodableValue(true));
            } else if (call.method_name() == "focus") {
              api->focus();
              result->Success(EncodableValue(true));
            } else if (call.method_name() == "stayOnTop") {
              const auto& args = std::get_if<EncodableMap>(call.arguments());
              bool stayOnTop{};
              for (auto& it : *args) {
                if ("stayOnTop" == std::get<std::string>(it.first) &&
                    std::holds_alternative<bool>(it.second)) {
                  stayOnTop = std::get<bool>(it.second);
                }
              }
              api->stayOnTop(stayOnTop);
              result->Success(EncodableValue(true));
            } else {
              result->NotImplemented();
            }
            return std::nullopt;
          });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

}  // namespace desktop_window_linux_plugin
