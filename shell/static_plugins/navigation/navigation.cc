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


#include "navigation.h"

#include <flutter/fml/logging.h>
#include <flutter/json_method_codec.h>

#include "engine.h"

void Navigation::OnPlatformMessage(const FlutterPlatformMessage* message,
                                   void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto codec = &flutter::JsonMethodCodec::GetInstance();
  auto obj = codec->DecodeMethodCall(message->message, message->message_size);
  auto method = obj->method_name();
  auto args = obj->arguments();

  if (method == kSelectSingleEntryHistory) {
    if (args->IsNull()) {
      FML_LOG(INFO) << "Navigation: Select Single Entry History";
      auto res = codec->EncodeSuccessEnvelope();
      engine->SendPlatformMessageResponse(message->response_handle, res->data(), res->size());
      return;
    }
  }
  else if (method == kRouteInformationUpdated) {
    if (args->HasMember("location") && args->HasMember("state") && args->HasMember("replace")) {
      RouteInformation info{};
      info.location = (*args)["location"].GetString();
      info.state = !(*args)["state"].IsNull() ? (*args)["state"].GetString() : "";
      info.replace = (*args)["replace"].GetBool();
      FML_LOG(INFO) << "Navigation: Route Information Updated"
                       "\n\tlocation: " << info.location <<
                       "\n\tstate: " << info.state <<
                       "\n\treplace: " << info.replace;
      auto res = codec->EncodeSuccessEnvelope();
      engine->SendPlatformMessageResponse(message->response_handle, res->data(), res->size());
      return;
    }
  }

  FML_LOG(ERROR) << "[Navigation] Unhandled: " << method;
  engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
}
