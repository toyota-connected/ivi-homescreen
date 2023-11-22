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

#include <cstring>
#include <memory>

#include <flutter/basic_message_channel.h>
#include <flutter/shell/platform/common/json_message_codec.h>
#include <xkbcommon/xkbcommon.h>

#include "engine.h"
#include "key_event.h"
#include "logging.h"

DelegateHandleKey::DelegateHandleKey(void* data,
                                     DelegateHandleKey::HandleKeyHook hook,
                                     FlutterKeyEventType type,
                                     uint32_t xkb_scancode,
                                     xkb_keysym_t keysym)
    : m_data(data),
      m_hook(hook),
      m_type(type),
      m_xkb_scancode(xkb_scancode),
      m_keysym(keysym) {}

void DelegateHandleKey::TriggerHook() {
  if (m_hook) {
    m_hook(m_data, m_type, m_xkb_scancode, m_keysym);
  }
}

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
  auto engine = reinterpret_cast<Engine*>(userdata);
  engine->SendPlatformMessageResponse(message->response_handle,
                                      message->message, message->message_size);
}

KeyEvent::FL_KEY_EV_RET_T KeyEvent::ParseReply(const uint8_t* reply,
                                               size_t reply_size) {
  auto ret = FL_KEY_EV_RET_FAILED;

  if (reply_size == 0) {
    SPDLOG_DEBUG("KeyEvent: BinaryReply: reply_size is 0");
    return ret;
  }

  auto decoded =
      flutter::JsonMessageCodec::GetInstance().DecodeMessage(reply, reply_size);
  if (decoded == nullptr) {
    /* could not decode */
    SPDLOG_DEBUG("KeyEvent: BinaryReply: could not decoded");
    return ret;
  }

  auto handled = decoded->FindMember(kHandled);
  if (handled == decoded->MemberEnd()) {
    spdlog::debug("KeyEvent: BinaryReply: could not found key \"{}\"",
                  kHandled);

    // check the reply contents
    for (auto& m : decoded->GetObject()) {
      spdlog::debug("key: {} type: {}", m.name.GetString(),
                    static_cast<int>(m.value.GetType()));
    }

    return ret;
  }

  if (!(handled->value.IsBool())) {
    SPDLOG_DEBUG("KeyEvent: BinaryReply: key \"{}\" is not bool. unexpected.",
                 kHandled);
    return ret;
  }

  ret =
      handled->value.GetBool() ? FL_KEY_EV_RET_HANDLED : FL_KEY_EV_RET_IGNORED;

  return ret;
}

void KeyEvent::SendKeyEvent(bool released,
                            xkb_keysym_t keysym,
                            uint32_t xkb_scancode,
                            uint32_t modifiers,
                            std::shared_ptr<DelegateHandleKey> delegate) {
  rapidjson::Document event(rapidjson::kObjectType);
  auto& allocator = event.GetAllocator();

  const uint32_t utf32_code = xkb_keysym_to_utf32(keysym);

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

  // flutter-engine will return the result which if a key event is handled or
  // ignored. If a key is handled, the key event must not be propagated to any
  // other plugins e.g. TextInput. If a key is ignored, the key event will be
  // propagated to other plugins e.g. TextInput. So, the reply from
  // flutter-engine needs to be received.
  channel_->Send(event, [delegate = std::move(delegate)](const uint8_t* reply,
                                                         size_t reply_size) {
    if (delegate == nullptr) {
      // delegate is not set, parsing the keyevent result is meaningless, so
      // skip
      return;
    }

    auto ret = ParseReply(reply, reply_size);

    switch (ret) {
      case FL_KEY_EV_RET_HANDLED:
        // skip any delegates if handled
        break;
      case FL_KEY_EV_RET_FAILED:
      case FL_KEY_EV_RET_IGNORED:
        // If parsing is failed, some issues might happen on the side of flutter
        // app. However, here, what issue happened cannot be got know. So, the
        // failure of parsing is determined as same as ignored.
        delegate->TriggerHook();
        break;
    }
  });
}

void KeyEvent::keyboard_handle_key(
    void* data,
    const FlutterKeyEventType eventType,
    const uint32_t xkb_scancode,
    const xkb_keysym_t keysym,
    std::shared_ptr<DelegateHandleKey> delegate) {
  const auto d = static_cast<KeyEvent*>(data);

  d->SendKeyEvent(eventType == kFlutterKeyEventTypeUp, keysym, xkb_scancode, 0,
                  std::move(delegate));
}

void KeyEvent::Send(const std::string& channel,
                    const uint8_t* message,
                    const size_t message_size,
                    const flutter::BinaryReply reply) const {
  last_binary_reply_ = reply;

  engine_->SendPlatformMessage(
      channel.c_str(), message, message_size,
      ReplyHandler,
      const_cast<KeyEvent*>(this));
}

void KeyEvent::SetMessageHandler(const std::string& channel,
                                 const flutter::BinaryMessageHandler handler) {
  last_message_handler_channel_ = channel;
  last_message_handler_ = handler;
}
