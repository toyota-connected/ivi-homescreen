/*
* @copyright Copyright (c) 2023 Toyota Connected North America
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "constants.h"

#include "engine.h"
#include "keyboard_manager.h"

void KeyboardManager::OnPlatformMessage(const FlutterPlatformMessage* message,
                                 void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::StandardMethodCodec::GetInstance();
  std::unique_ptr<std::vector<uint8_t>> result =
      codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();

  /* Get Keyboard State */
  if (method == kGetKeyboardState) {
    SPDLOG_DEBUG("({}) {}", engine->GetIndex(), kGetKeyboardState);
    flutter::EncodableValue value(flutter::EncodableMap{
        {flutter::EncodableValue(0),
         flutter::EncodableValue(0)},
    });
    result = codec.EncodeSuccessEnvelope(&value);
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
