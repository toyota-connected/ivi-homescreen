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


#include "accessibility.h"

#include <flutter/fml/logging.h>
#include <flutter/standard_message_codec.h>

#include "engine.h"
#include "hexdump.h"

static void PrintMessageAsHexDump(const FlutterPlatformMessage* msg) {
  std::stringstream ss;
  ss << Hexdump(msg->message, msg->message_size);
  FML_DLOG(INFO) << "Channel: \"" << msg->channel << "\"\n" << ss.str();
}

void Accessibility::OnPlatformMessage(const FlutterPlatformMessage* message,
                                      void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto codec = &flutter::StandardMessageCodec::GetInstance();
  auto obj = codec->DecodeMessage(message->message, message->message_size);

  if (obj->IsMap()) {
    auto map = obj->MapValue();
    std::string type = map[flutter::EncodableValue("type")].StringValue();
    flutter::EncodableMap data =
        map[flutter::EncodableValue("data")].MapValue();
    std::string msg = data[flutter::EncodableValue("message")].StringValue();

    FML_DLOG(INFO) << "Accessibility: type: " << type << ", message: " << msg;

  } else {
    FML_LOG(ERROR) << "Unhandled Accessibility Message";
    PrintMessageAsHexDump(message);
  }

  engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
}
