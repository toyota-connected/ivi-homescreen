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

  if (obj->method_name() == "routeUpdated") {
    auto args = obj->arguments();
    std::string previousRouteName;
    std::string routeName;

    auto itr = args->FindMember("previousRouteName");
    if (itr != args->MemberEnd()) {
      if (!itr->value.IsNull()) {
        previousRouteName = itr->value.GetString();
      }
    }
    itr = args->FindMember("routeName");
    if (itr != args->MemberEnd()) {
      if (!itr->value.IsNull()) {
        routeName = itr->value.GetString();
      }
    }
    FML_DLOG(INFO) << "Navigation: routeName=" << routeName
                   << ", previousRouteName=" << previousRouteName;
  } else {
    FML_LOG(ERROR) << "Navigation Channel Unhandled: " << obj->method_name();
  }

  engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
}
