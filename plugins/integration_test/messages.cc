/*
 * Copyright 2024 Toyota Connected North America
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

namespace integration_test_plugin {

using flutter::BasicMessageChannel;
using flutter::CustomEncodableValue;
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;
using flutter::MethodCall;
using flutter::MethodResult;

// Method Constants
static constexpr char kArgResults[] = "results";
static constexpr char kResultSuccess[] = "success";

/// The codec used by IntegrationTestApi.
const flutter::StandardMethodCodec& IntegrationTestApi::GetCodec() {
  return flutter::StandardMethodCodec::GetInstance();
}

// Sets up an instance of `IntegrationTest` to handle messages through the
// `binary_messenger`.
void IntegrationTestApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                               IntegrationTestApi* api) {
  {
    auto channel = std::make_unique<flutter::MethodChannel<>>(
        binary_messenger, "plugins.flutter.io/integration_test", &GetCodec());
    if (api != nullptr) {
      channel->SetMethodCallHandler(
          [api](const flutter::MethodCall<EncodableValue>& call,
                std::unique_ptr<flutter::MethodResult<EncodableValue>> result) {
            const auto& method = call.method_name();
            SPDLOG_DEBUG("[IntegrationTest] {}", method);

            if (method == "allTestsFinished") {
              auto args = std::get_if<EncodableMap>(call.arguments());
              if (args->empty()) {
                return result->Error("argument_error", "no arguments provided");
              }
              api->ArgResults(*args);
              return result->Success();
            } else if (method == "convertFlutterSurfaceToImage") {
              return result->Error(
                  "Could not convert to image, Not implemented yet");
            } else if (method == "revertFlutterImage") {
              return result->Error(
                  "Could not revert Flutter image, Not implemented yet");
            } else if (method == "captureScreenshot") {
              return result->Error(
                  "Could not capture screenshot, Not implemented yet");
            } else if (method == "scheduleFrame") {
              return result->Error(
                  "Could not schedule frame, Not implemented yet");
            }
            return result->Success();
          });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

}  // namespace integration_test_plugin