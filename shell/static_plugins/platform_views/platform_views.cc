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


#include "platform_views.h"

#include <flutter/fml/logging.h>
#include <flutter/shell/platform/common/json_method_codec.h>

#include "engine.h"

void PlatformViews::OnPlatformMessage(const FlutterPlatformMessage* message,
                                      void* userdata) {
  (void)userdata;
  rapidjson::Document document;
  document.Parse(reinterpret_cast<const char*>(message->message),
                 message->message_size);
  if (document.HasParseError() || !document.IsObject()) {
    FML_LOG(ERROR) << "Could not parse document";
    return;
  }
  auto root = document.GetObject();
  auto method = root.FindMember("method");
  if (method == root.MemberEnd() || !method->value.IsString()) {
    return;
  }

  std::string msg;
  msg.assign(reinterpret_cast<const char*>(message->message),
             message->message_size);
  FML_DLOG(INFO) << "PlatformViews: " << method->value.GetString();

  if (method->value == "View.enableWireframe") {
    auto args_it = root.FindMember("args");
    if (args_it == root.MemberEnd() || !args_it->value.IsObject()) {
      FML_DLOG(INFO) << "No arguments found.";
      return;
    }
    const auto& args = args_it->value;

    auto enable = args.FindMember("enable");
    if (!enable->value.IsBool()) {
      FML_DLOG(INFO) << "Argument 'enable' is not a bool";
      return;
    }

    FML_DLOG(INFO) << "wireframe_enabled_callback goes here";
  } else {
    FML_DLOG(INFO) << "Unknown " << message->channel << " method "
                   << method->value.GetString();
  }
}
