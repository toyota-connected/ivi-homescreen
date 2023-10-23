// Copyright 2020 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "go_router.h"

#include <flutter/shell/platform/common/json_method_codec.h>

#include "engine.h"

void GoRouter::OnPlatformMessage(const FlutterPlatformMessage* message,
                                 void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::JsonMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  if (method == kSelectSingleEntryHistory) {
    spdlog::info("({}) [go_router] Select Single Entry History",
                 engine->GetIndex());
    result = codec.EncodeSuccessEnvelope();
  } else if (method == kSelectMultiEntryHistory) {
    spdlog::info("({}) [go_router] Select Multiple Entry History",
                 engine->GetIndex());
    result = codec.EncodeSuccessEnvelope();
  } else if (method == kMethodSystemNavigatorPop) {
    spdlog::info("({}) [go_router] Pop", engine->GetIndex());
    result = codec.EncodeSuccessEnvelope();
  } else if (method == kRouteInformationUpdated) {
    auto args = obj->arguments();
    std::string uri;
    if (args->HasMember("uri") && (*args)["uri"].IsString()) {
      uri = (*args)["uri"].GetString();
    }
    std::string location;
    if (args->HasMember("location") && (*args)["location"].IsString()) {
      location = (*args)["location"].GetString();
    }
    bool replace{};
    if (args->HasMember("replace") && (*args)["replace"].IsBool()) {
      replace = (*args)["replace"].GetBool();
    }
    if (!location.empty()) {
      spdlog::info(
          "({}) [go_router] Route Information Updated"
          "\n\tlocation: {} \n\treplace: {}",
          engine->GetIndex(), location, replace);
    } else {
      spdlog::info(
          "({}) [go_router] Route Information Updated"
          "\n\turi: {} \n\treplace: {}",
          engine->GetIndex(), uri, replace);
    }

    if (args->HasMember("state") && (*args)["state"].IsObject()) {
      spdlog::info("({}) [go_router] State Object Valid", engine->GetIndex());
    }

    result = codec.EncodeSuccessEnvelope();
  } else {
    SPDLOG_DEBUG("({}) [go_router] {} is unhandled", engine->GetIndex(),
                 method);
    result = codec.EncodeErrorEnvelope("unhandled_method", "unhandled Method");
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
