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

#include "desktop_window.h"

#include <flutter/standard_method_codec.h>

#include "engine.h"
#include "logging.h"

void DesktopWindow::OnPlatformMessage(const FlutterPlatformMessage* message,
                                      void* userdata) {
  const auto engine = static_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  const auto obj =
      codec.DecodeMethodCall(message->message, message->message_size);

  const auto method = obj->method_name();

  if (method == "desktop_window") {
    if (!obj->arguments()->IsNull()) {
      const auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

      SPDLOG_DEBUG("desktop_window");

      for (auto const& it : *args) {
        auto key = std::get<std::string>(it.first);
        SPDLOG_DEBUG(key);
      }

      int64_t setMinWindowSize = 0;
      auto it = args->find(flutter::EncodableValue("setMinWindowSize"));
      if (it != args->end()) {
        const flutter::EncodableValue encodedValue = it->second;
        setMinWindowSize = encodedValue.LongValue();
      }

      int64_t width = 0;
      it = args->find(flutter::EncodableValue("width"));
      if (it != args->end()) {
        const flutter::EncodableValue encodedValue = it->second;
        width = encodedValue.LongValue();
      }

      int64_t height = 0;
      it = args->find(flutter::EncodableValue("height"));
      if (it != args->end()) {
        const flutter::EncodableValue encodedValue = it->second;
        height = encodedValue.LongValue();
      }

      (void)setMinWindowSize;
      (void)width;
      (void)height;
      SPDLOG_DEBUG("setMinWindowSize: {}", setMinWindowSize);
      SPDLOG_DEBUG("width: {}", width);
      SPDLOG_DEBUG("height: {}", height);
    }
  }

  const auto result = codec.EncodeSuccessEnvelope();
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
