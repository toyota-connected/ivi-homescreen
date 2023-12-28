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

#include "logging/logging.h"

namespace go_router_plugin {

using flutter::BasicMessageChannel;
using flutter::CustomEncodableValue;
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;
using flutter::MethodCall;
using flutter::MethodResult;

/// The codec used by DesktopWindowLinuxApi.
const flutter::JsonMethodCodec& GoRouterApi::GetCodec() {
  return flutter::JsonMethodCodec::GetInstance();
}

// Sets up an instance of `DesktopWindowLinuxApi` to handle messages through the
// `binary_messenger`.
void GoRouterApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                        GoRouterApi* api) {
  {
    auto channel =
        std::make_unique<flutter::MethodChannel<rapidjson::Document>>(
            binary_messenger, "flutter/navigation", &GetCodec());
    if (api != nullptr) {
      channel->SetMethodCallHandler(
          [api](const flutter::MethodCall<rapidjson::Document>& call,
                std::unique_ptr<flutter::MethodResult<rapidjson::Document>>
                    result) {
            const auto& method = call.method_name();
            const auto args = call.arguments();
            if (method == "selectSingleEntryHistory") {
              spdlog::debug("selectSingleEntryHistory");
            } else if (method == "selectMultiEntryHistory") {
              spdlog::debug("selectMultiEntryHistory");
            } else if (method == "routeInformationUpdated") {
              std::string uri;
              if (args->HasMember("uri") && (*args)["uri"].IsString()) {
                uri = (*args)["uri"].GetString();
              }
              std::string location;
              if (args->HasMember("location") &&
                  (*args)["location"].IsString()) {
                location = (*args)["location"].GetString();
              }
              bool replace{};
              if (args->HasMember("replace") && (*args)["replace"].IsBool()) {
                replace = (*args)["replace"].GetBool();
              }

              if (!location.empty()) {
                spdlog::info(
                    "[go_router] Route Information Updated"
                    "\n\tlocation: {} \n\treplace: {}");
              } else {
                spdlog::info(
                    "[go_router] Route Information Updated"
                    "\n\turi: {} \n\treplace: {}",
                    uri, replace);
              }
              if (args->HasMember("state") && (*args)["state"].IsObject()) {
                spdlog::info("[go_router] State Object Valid");
              }
            } else if (method == "SystemNavigator.pop") {
              spdlog::debug("SystemNavigator.pop");
            } else {
              result->NotImplemented();
              return;
            }
            return result->Success();
          });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

}  // namespace go_router_plugin
