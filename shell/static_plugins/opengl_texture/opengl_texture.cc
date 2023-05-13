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

#include <flutter/standard_method_codec.h>

#include "engine.h"

#include "textures/egl/texture_egl.h"

void OpenGlTexture::OnPlatformMessage(const FlutterPlatformMessage* message,
                                      void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  std::unique_ptr<std::vector<uint8_t>> result =
      codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  if (method == "create") {
    if (!obj->arguments()->IsNull()) {
      auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

      int64_t textureId = 0;
      auto it = args->find(flutter::EncodableValue("textureId"));
      if (it != args->end() && !it->second.IsNull()) {
        flutter::EncodableValue encodedValue = it->second;
        textureId = encodedValue.LongValue();
      }

      double width = 0;
      it = args->find(flutter::EncodableValue("width"));
      if (it != args->end() && !it->second.IsNull()) {
        width = std::get<double>(it->second);
      }

      double height = 0;
      it = args->find(flutter::EncodableValue("height"));
      if (it != args->end() && !it->second.IsNull()) {
        height = std::get<double>(it->second);
      }

      if (0 == textureId || 0 == width || 0 == height) {
        result = codec.EncodeErrorEnvelope(
            "argument_error", "textureId, width and height must be non-zero");
      } else {
        // cast size to that what Wayland uses
        auto value = TextureEgl::GetInstance().Create(
            engine, textureId, static_cast<int32_t>(width),
            static_cast<int32_t>(height), args);

        result = codec.EncodeSuccessEnvelope(&value);
      }
    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  } else if (method == "dispose") {
    if (!obj->arguments()->IsNull()) {
      auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

      int64_t textureId = 0;
      auto it = args->find(flutter::EncodableValue("textureId"));
      if (it != args->end()) {
        flutter::EncodableValue encodedValue = it->second;
        textureId = encodedValue.LongValue();
      }

      engine->TextureDispose(textureId);

      result = codec.EncodeSuccessEnvelope();
    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  }
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
