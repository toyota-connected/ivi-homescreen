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
#include "rapidjson/writer.h"

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
          [](const flutter::MethodCall<rapidjson::Document>& call,
                std::unique_ptr<flutter::MethodResult<rapidjson::Document>>
                    result) {
            const auto& method = call.method_name();
            const auto args = call.arguments();
            rapidjson::StringBuffer buffer;
            buffer.Clear();
            rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
            args->Accept(writer);
            spdlog::debug("[go_router] {}", buffer.GetString());

            if (method == "selectSingleEntryHistory") {
              spdlog::debug("[go_router] selectSingleEntryHistory");
            } else if (method == "selectMultiEntryHistory") {
              spdlog::debug("[go_router] selectMultiEntryHistory");
            } else if (method == "routeInformationUpdated") {
              spdlog::debug("[go_router] routeInformationUpdated");
              std::string uri;
              if (args->HasMember("uri") && (*args)["uri"].IsString()) {
                uri = (*args)["uri"].GetString();
                spdlog::debug("\turi: {}", uri);
              }
              bool replace{};
              if (args->HasMember("replace") && (*args)["replace"].IsBool()) {
                replace = (*args)["replace"].GetBool();
                spdlog::debug("\treplace: {}", replace);
              }
              std::string codec;
              std::string encoded;
              if (args->HasMember("state") && (*args)["state"].IsObject()) {
                auto state = (*args)["state"].GetObject();

                std::string location;
                if (state.HasMember("location") &&
                    state["location"].IsString()) {
                  location = state["location"].GetString();
                  spdlog::debug("\tlocation: {}", location);
                }
                if (state.HasMember("state") && state["state"].IsObject()) {
                  auto state1 = state["state"].GetObject();

                  if (state1.HasMember("codec") && state1["codec"].IsString()) {
                    codec = state1["codec"].GetString();
                    spdlog::debug("\tstate::codec: {}", codec);
                  }
                  if (state1.HasMember("encoded") &&
                      state1["encoded"].IsString()) {
                    encoded = state1["encoded"].GetString();
                    spdlog::debug("\tstate::encoded: {}", encoded);
                  }
                }
                if (args->HasMember("imperativeMatches") && (*args)["imperativeMatches"].IsArray()) {
                  auto val = (*args)["imperativeMatches"].GetArray();
                }
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
