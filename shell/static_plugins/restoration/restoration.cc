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

#include "restoration.h"

#include <flutter/fml/logging.h>
#include <flutter/standard_method_codec.h>

#include "engine.h"

void Restoration::OnPlatformMessage(const FlutterPlatformMessage* message,
                                    void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  if (method == kMethodGet) {
    FML_DLOG(INFO) << "(" << engine->GetIndex() << ") Restoration: Get";
  } else {
    FML_DLOG(ERROR) << "(" << engine->GetIndex() << ") Restoration: " << method
                    << " is unhandled";
  }

  engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
}
