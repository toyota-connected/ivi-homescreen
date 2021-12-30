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

#include <flutter/fml/logging.h>
#include <flutter/standard_method_codec.h>

#include "engine.h"

void MouseCursor::OnPlatformMessage(const FlutterPlatformMessage* message,
                                    void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto codec = &flutter::StandardMethodCodec::GetInstance();
  auto obj = codec->DecodeMethodCall(message->message, message->message_size);

  if (obj->method_name() == "activateSystemCursor") {
    if (obj->arguments() && obj->arguments()->IsMap()) {
      const flutter::EncodableMap& args = obj->arguments()->MapValue();

      int32_t device = 0;
      auto it = args.find(flutter::EncodableValue("device"));
      if (it != args.end()) {
        device = it->second.IntValue();
      }

      std::string kind;
      it = args.find(flutter::EncodableValue("kind"));
      if (it != args.end()) {
        kind = it->second.StringValue();
      }
#if 0 //TODO - call into display and set active cursor
      FML_DLOG(INFO) << "activateSystemCursor: device=" << device
                     << ", kind=" << kind;
#endif
      auto val = flutter::EncodableValue(true);
      auto result = codec->EncodeSuccessEnvelope(&val);
      engine->SendPlatformMessageResponse(message->response_handle,
                                          result->data(), result->size());
      return;
    }
  } else {
    FML_DLOG(INFO) << "MouseCursor: " << obj->method_name() << " is unhandled";
  }

  engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
}
