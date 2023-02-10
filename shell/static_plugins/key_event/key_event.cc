/*
 * @copyright Copyright (c) 2022 Woven Alpha, Inc.
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

#include <flutter/fml/logging.h>

#include <cstring>
#include <memory>

#include <flutter/basic_message_channel.h>
#include <flutter/shell/platform/common/json_message_codec.h>
#include <xkbcommon/xkbcommon.h>

#include "engine.h"
#include "key_event.h"

KeyEvent::KeyEvent()
    : channel_(
          std::make_unique<flutter::BasicMessageChannel<rapidjson::Document>>(
              this,
              kChannelName,
              &flutter::JsonMessageCodec::GetInstance())) {}

void KeyEvent::SetEngine(const std::shared_ptr<Engine>& engine) {
  if (engine) {
    engine_ = engine;
    engine->SetKeyEvent(this);
  }
}

void KeyEvent::OnPlatformMessage(const FlutterPlatformMessage* message,
                                 void* userdata) {
  FML_DLOG(INFO) << "RECV: KeyEvent::OnPlatformMessage(): \""
                 << message->message << "\"";
}

void KeyEvent::SendKeyEvent(bool released,
                            xkb_keysym_t keysym,
                            uint32_t xkb_scancode,
                            uint32_t modifiers) {
  rapidjson::Document event(rapidjson::kObjectType);
  auto& allocator = event.GetAllocator();

  uint32_t utf32_code = xkb_keysym_to_utf32(keysym);

  event.AddMember(kKeyCodeKey, keysym, allocator);
  event.AddMember(kKeyMapKey, kLinuxKeyMap, allocator);
  event.AddMember(kToolkitKey, kValueToolkitGtk, allocator);
  event.AddMember(kScanCodeKey, xkb_scancode, allocator);
  event.AddMember(kModifiersKey, modifiers, allocator);
  if (utf32_code)
    event.AddMember(kUnicodeScalarValues, utf32_code, allocator);
  if (released) {
    event.AddMember(kTypeKey, kKeyUp, allocator);
  } else {
    event.AddMember(kTypeKey, kKeyDown, allocator);
  }

  channel_->Send(event);
}

void KeyEvent::keyboard_handle_key(void* data,
                                   FlutterKeyEventType eventType,
                                   uint32_t xkb_scancode,
                                   xkb_keysym_t keysym) {
  auto d = reinterpret_cast<KeyEvent*>(data);

  d->SendKeyEvent(eventType == kFlutterKeyEventTypeUp, keysym, xkb_scancode, 0);
}

void KeyEvent::Send(const std::string& channel,
                    const uint8_t* message,
                    size_t message_size,
                    flutter::BinaryReply reply) const {
  engine_->SendPlatformMessage(channel.c_str(), message, message_size);
}

void KeyEvent::SetMessageHandler(const std::string& channel,
                                 flutter::BinaryMessageHandler handler) {}
