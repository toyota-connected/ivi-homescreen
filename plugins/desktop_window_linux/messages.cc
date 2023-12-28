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
          [api](const MethodCall<EncodableValue>& methodCall,
                std::unique_ptr<MethodResult<EncodableValue>> result) {
            if (methodCall.method_name() == "setMinWindowSize") {
              const auto& args =
                  std::get_if<EncodableMap>(methodCall.arguments());

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

              try {
                api->SetMinWindowSize(
                    width, height,
                    [result =
                         result.get()](std::optional<FlutterError>&& output) {
                      if (output.has_value()) {
                        result->Error(output->code(), output->message(),
                                      output->details());
                        return;
                      }
                    });
              } catch (const std::exception& exception) {
                result->Error(exception.what());
              }
              result->Success();
            } else {
              result->NotImplemented();
            }
          });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

}  // namespace desktop_window_linux_plugin
