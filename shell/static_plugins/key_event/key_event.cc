/*
 * @copyright Copyright (c) 2022 Woven Alpha, Inc.
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

#include <memory>
#include <string>
#include <cerrno>
#include <cstring>

#include <xkbcommon/xkbcommon.h>
#include <rapidjson/document.h>
#include <flutter/basic_message_channel.h>
#include <flutter/binary_messenger.h>
#include <flutter/shell/platform/common/json_message_codec.h>

#include "key_event.h"
#include "key_mapping.h"
#include "engine.h"

KeyEvent::KeyEvent()
    : channel_(std::make_unique<flutter::BasicMessageChannel<rapidjson::Document>>(
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
  FML_LOG(INFO) << "RECV: KeyEvent::OnPlatformMessage(): \"" << message->message << "\"";
}

void KeyEvent::SendKeyEvent(bool pressed,
                            xkb_keysym_t sym,
                            uint32_t key,
                            uint32_t modifiers) {
  rapidjson::Document event(rapidjson::kObjectType);
  auto& allocator = event.GetAllocator();

  uint32_t utf32_code = xkb_keysym_to_utf32(sym);
  uint32_t scancode;

  if (map_xkb_key_to_fl_snancode.find(key) != map_xkb_key_to_fl_snancode.end()) {
    scancode = map_xkb_key_to_fl_snancode.at(key);
  } else {
    scancode = utf32_code;
  }

  event.AddMember(kKeyCodeKey, scancode, allocator);
  event.AddMember(kKeyMapKey, kLinuxKeyMap, allocator);
  event.AddMember(kToolkitKey, kGLFWKey, allocator);
  event.AddMember(kScanCodeKey, scancode, allocator);
  event.AddMember(kModifiersKey, modifiers, allocator);
  if (utf32_code)
    event.AddMember(kUnicodeScalarValues, utf32_code, allocator);
  if (pressed) {
    event.AddMember(kTypeKey, kKeyDown, allocator);
  } else {
    event.AddMember(kTypeKey, kKeyUp, allocator);
  }

  channel_->Send(event);
}

void KeyEvent::keyboard_handle_key(void *data,
                                   FlutterKeyEventType type,
                                   uint32_t code,
                                   xkb_keysym_t sym) {
  auto d = reinterpret_cast<KeyEvent*>(data);

  d->SendKeyEvent(type == kFlutterKeyEventTypeUp ? false : true, sym, code - 8, 0);
}

void KeyEvent::Send(const std::string& channel,
                    const uint8_t* message,
                    size_t message_size,
                    flutter::BinaryReply reply) const {
  engine_->SendPlatformMessage(channel.c_str(), message, message_size);
}

void KeyEvent::SetMessageHandler(const std::string& channel,
                                 flutter::BinaryMessageHandler handler) {}
