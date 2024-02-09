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

#include <optional>
#include <string>

#include "plugins/common/common.h"

namespace mouse_cursor_plugin {

using flutter::BasicMessageChannel;
using flutter::CustomEncodableValue;
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;
using flutter::MethodCall;
using flutter::MethodResult;

/// The codec used by MouseCursorApi.
const flutter::StandardMethodCodec& MouseCursorApi::GetCodec() {
  return flutter::StandardMethodCodec::GetInstance();
}

// Sets up an instance of `MouseCursorApi` to handle messages through the
// `binary_messenger`.
void MouseCursorApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                           MouseCursorApi* api) {
  {
    auto channel = std::make_unique<flutter::MethodChannel<>>(
        binary_messenger, "flutter/mousecursor",
        &flutter::StandardMethodCodec::GetInstance());
    if (api != nullptr) {
      channel->SetMethodCallHandler(
          [api](const flutter::MethodCall<EncodableValue>& call,
                const std::unique_ptr<flutter::MethodResult<EncodableValue>>&
                    result) {
            const auto& method = call.method_name();
            if (method == "activateSystemCursor") {
              const auto& args = std::get_if<EncodableMap>(call.arguments());
              int32_t device = 0;
              std::string kind;
              for (auto& it : *args) {
                if ("device" == std::get<std::string>(it.first) &&
                    std::holds_alternative<int32_t>(it.second)) {
                  device = std::get<int32_t>(it.second);
                }
                if ("kind" == std::get<std::string>(it.first) &&
                    std::holds_alternative<std::string>(it.second)) {
                  kind = std::get<std::string>(it.second);
                }
              }
              return result->Success(flutter::EncodableValue(
                  api->ActivateSystemCursor(device, kind)));
            } else {
              return result->NotImplemented();
            }
          });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

}  // namespace mouse_cursor_plugin
