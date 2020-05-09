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


#include "opengl_texture.h"

#include <flutter/fml/logging.h>
#include <flutter/standard_method_codec.h>

#include "engine.h"

void OpenGlTexture::OnPlatformMessage(const FlutterPlatformMessage* message,
                                      void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto codec = &flutter::StandardMethodCodec::GetInstance();
  auto obj = codec->DecodeMethodCall(message->message, message->message_size);

  if (obj->method_name() == "create") {
    if (obj->arguments() && obj->arguments()->IsMap()) {
      const flutter::EncodableMap& args = obj->arguments()->MapValue();

      int64_t textureId = 0;
      auto it = args.find(flutter::EncodableValue("textureId"));
      if (it != args.end()) {
        textureId = it->second.LongValue();
      }

      double width = 0;
      it = args.find(flutter::EncodableValue("width"));
      if (it != args.end()) {
        width = it->second.DoubleValue();
      }

      double height = 0;
      it = args.find(flutter::EncodableValue("height"));
      if (it != args.end()) {
        height = it->second.DoubleValue();
      }

      // cast size to that what Wayland uses
      textureId = engine->TextureCreate(textureId, static_cast<int32_t>(width),
                                        static_cast<int32_t>(height));

      flutter::EncodableValue value(textureId);
      auto encoded = codec->EncodeSuccessEnvelope(&value);
      engine->SendPlatformMessageResponse(message->response_handle,
                                          encoded->data(), encoded->size());
      return;
    }
  } else if (obj->method_name() == "dispose") {
    if (obj->arguments() && obj->arguments()->IsMap()) {
      const flutter::EncodableMap& args = obj->arguments()->MapValue();

      int64_t textureId = 0;
      auto it = args.find(flutter::EncodableValue("textureId"));
      if (it != args.end()) {
        textureId = it->second.IntValue();
      }

      engine->TextureDispose(textureId);

      auto encoded = codec->EncodeSuccessEnvelope();
      engine->SendPlatformMessageResponse(message->response_handle,
                                          encoded->data(), encoded->size());
      return;
    }
  }
  engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
}
