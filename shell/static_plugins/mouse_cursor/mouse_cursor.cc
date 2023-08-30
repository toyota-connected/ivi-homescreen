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

#include "mouse_cursor.h"

#include <flutter/standard_method_codec.h>

#include "engine.h"
#include "logging.h"

#include <iostream>

void MouseCursor::OnPlatformMessage(const FlutterPlatformMessage* message,
                                    void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  if (method == kMethodActivateSystemCursor) {
    if (!obj->arguments()->IsNull()) {
      auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

      int32_t device = 0;
      auto it = args->find(flutter::EncodableValue("device"));
      if (it != args->end() && !it->second.IsNull()) {
        device = std::get<int32_t>(it->second);
      }

      std::string kind;
      it = args->find(flutter::EncodableValue("kind"));
      if (it != args->end() && !it->second.IsNull()) {
        kind = std::get<std::string>(it->second);
      }

      auto val =
          flutter::EncodableValue(engine->ActivateSystemCursor(device, kind));
      result = codec.EncodeSuccessEnvelope(&val);
    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  } else {
    SPDLOG_DEBUG("MouseCursor: {} is unhandled", method);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
