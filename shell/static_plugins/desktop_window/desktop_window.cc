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
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  if (method == "desktop_window") {
    if (!obj->arguments()->IsNull()) {
      auto args = std::get_if<flutter::EncodableMap>(obj->arguments());

      FML_DLOG(INFO) << "desktop_window";

      for (auto it : *args) {
        auto key = std::get<std::string>(it.first);
        FML_DLOG(INFO) << key;
      }

      int64_t setMinWindowSize = 0;
      auto it = args->find(flutter::EncodableValue("setMinWindowSize"));
      if (it != args->end()) {
        flutter::EncodableValue encodedValue = it->second;
        setMinWindowSize = encodedValue.LongValue();
      }

      int64_t width = 0;
      it = args->find(flutter::EncodableValue("width"));
      if (it != args->end()) {
        flutter::EncodableValue encodedValue = it->second;
        width = encodedValue.LongValue();
      }

      int64_t height = 0;
      it = args->find(flutter::EncodableValue("height"));
      if (it != args->end()) {
        flutter::EncodableValue encodedValue = it->second;
        height = encodedValue.LongValue();
      }

      FML_DLOG(INFO) << "setMinWindowSize: " << setMinWindowSize;
      FML_DLOG(INFO) << "width: " << width;
      FML_DLOG(INFO) << "height: " << height;
    }
  }

  result = codec.EncodeSuccessEnvelope();
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
