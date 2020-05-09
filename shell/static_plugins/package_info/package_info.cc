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


#include "package_info.h"

#include <flutter/fml/logging.h>
#include <flutter/standard_method_codec.h>

#include "engine.h"
#include "hexdump.h"

void PackageInfo::OnPlatformMessage(const FlutterPlatformMessage* message,
                                    void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto codec = &flutter::StandardMethodCodec::GetInstance();
  auto obj = codec->DecodeMethodCall(message->message, message->message_size);

  if (obj->method_name() == "getAll") {
    FML_DLOG(INFO) << "PackageInfo: getAll";

    flutter::EncodableValue value(flutter::EncodableMap{
        {flutter::EncodableValue("appName"),
         flutter::EncodableValue("Toyota Connected")},
        {flutter::EncodableValue("packageName"),
         flutter::EncodableValue("Wayland Embedder")},
        {flutter::EncodableValue("version"), flutter::EncodableValue("0.1.0")},
        {flutter::EncodableValue("buildNumber"),
         flutter::EncodableValue("2")}});

    auto encoded = codec->EncodeSuccessEnvelope(&value);
    engine->SendPlatformMessageResponse(message->response_handle,
                                        encoded->data(), encoded->size());
    return;
  } else {
    FML_LOG(ERROR) << "Unhandled PackageInfo Message";
    std::stringstream ss;
    ss << Hexdump(message->message, message->message_size);
    FML_DLOG(INFO) << "Channel: \"" << message->channel << "\"\n" << ss.str();
  }

  engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
}