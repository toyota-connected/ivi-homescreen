// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/platform/homescreen/key_event_handler.h"

#include <string>

#include "asio/post.hpp"
#include "flutter/shell/platform/common/json_message_codec.h"
#include "shell/engine.h"
#include "shell/platform/homescreen/key_mapping.h"
#include "shell/platform/homescreen/text_input_plugin.h"
#include "shell/task_runner.h"

static constexpr char kChannelName[] = "flutter/keyevent";

static constexpr char kKeyCodeKey[] = "keyCode";
static constexpr char kKeyMapKey[] = "keymap";
static constexpr char kScanCodeKey[] = "scanCode";
static constexpr char kModifiersKey[] = "modifiers";
static constexpr char kTypeKey[] = "type";
static constexpr char kToolkitKey[] = "toolkit";
static constexpr char kUnicodeScalarValues[] = "unicodeScalarValues";

static constexpr char kLinuxKeyMap[] = "linux";
static constexpr char kValueToolkitGtk[] = "gtk";

static constexpr char kKeyUp[] = "keyup";
static constexpr char kKeyDown[] = "keydown";

namespace {

// Per-event callback context heap-allocated for the embedder API call.
struct KeyEventCallbackData {
  flutter::TextInputPlugin* text_input;
  std::string character;  // UTF-8; empty for non-printable / key-up events
  xkb_keysym_t keysym;
  uint32_t xkb_scancode;
  uint32_t modifiers;
};

// Converts a UTF-32 codepoint to a UTF-8 string (for FlutterKeyEvent.character).
std::string Utf32ToUtf8(const uint32_t codepoint) {
  std::string out;
  if (codepoint <= 0x7f) {
    out += static_cast<char>(codepoint);
  } else if (codepoint <= 0x7ff) {
    out += static_cast<char>(0xc0 | (codepoint >> 6));
    out += static_cast<char>(0x80 | (codepoint & 0x3f));
  } else if (codepoint <= 0xffff) {
    out += static_cast<char>(0xe0 | (codepoint >> 12));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (codepoint & 0x3f));
  } else if (codepoint <= 0x10ffff) {
    out += static_cast<char>(0xf0 | (codepoint >> 18));
    out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (codepoint & 0x3f));
  }
  return out;
}

// Callback invoked by the Flutter engine after processing a key event via the
// embedder API. If the event was not handled AND it produced a printable
// character, fall back to the TextInputPlugin for text insertion.
void OnKeyEventResponse(bool handled, void* user_data) {
  auto* data = static_cast<KeyEventCallbackData*>(user_data);
  if (!handled && !data->character.empty() && data->text_input != nullptr) {
    data->text_input->KeyboardHook(/*released=*/false, data->keysym,
                                   data->xkb_scancode, data->modifiers);
  }
  delete data;
}

}  // namespace

namespace flutter {

KeyEventHandler::KeyEventHandler(flutter::BinaryMessenger* messenger)
    : channel_(
          std::make_unique<flutter::BasicMessageChannel<rapidjson::Document>>(
              messenger,
              kChannelName,
              &flutter::JsonMessageCodec::GetInstance())) {}

KeyEventHandler::~KeyEventHandler() = default;

void KeyEventHandler::CharHook(unsigned int /* code_point */) {}

void KeyEventHandler::KeyboardHook(const bool released,
                                   const xkb_keysym_t keysym,
                                   const uint32_t xkb_scancode,
                                   const uint32_t modifiers) {
  const uint64_t physical = key_mapping::XkbScancodeToPhysicalKey(xkb_scancode);
  const uint32_t utf32 = xkb_keysym_to_utf32(keysym);

  // -----------------------------------------------------------------------
  // Determine event type and logical key, tracking down/up pairs so that
  // key-up always sends the same logical key as the corresponding key-down
  // (utf32 is 0 on release which would otherwise yield a different value).
  // -----------------------------------------------------------------------
  FlutterKeyEventType type;
  uint64_t logical;

  if (!released) {
    const bool already_pressed = pressed_logical_keys_.count(physical) > 0;
    type = already_pressed ? kFlutterKeyEventTypeRepeat
                           : kFlutterKeyEventTypeDown;
    logical = key_mapping::KeysymToLogicalKey(keysym, utf32);
    pressed_logical_keys_[physical] = logical;
  } else {
    type = kFlutterKeyEventTypeUp;
    const auto it = pressed_logical_keys_.find(physical);
    logical = (it != pressed_logical_keys_.end())
                  ? it->second
                  : key_mapping::KeysymToLogicalKey(keysym, utf32);
    pressed_logical_keys_.erase(physical);
  }

  // -----------------------------------------------------------------------
  // 1. Send via embedder API (FlutterEngineSendKeyEvent) — primary path for
  //    HardwareKeyboard / Focus.onKeyEvent / Shortcuts in the framework.
  // -----------------------------------------------------------------------
  if (engine_ != nullptr) {
    // Build character string (only for key-down/repeat of printable chars).
    std::string character_str;
    if (!released && utf32 >= 0x20 && utf32 != 0x7f) {
      character_str = Utf32ToUtf8(utf32);
    }

    // Heap-allocate event data — ownership passes to OnKeyEventResponse.
    auto* cb_data = new KeyEventCallbackData{
        text_input_, character_str, keysym, xkb_scancode, modifiers};

    // Copy for capture (FlutterKeyEvent holds a char* into character_str, so
    // keep both alive in the lambda until the post fires).
    FlutterKeyEvent flutter_event{};
    flutter_event.struct_size = sizeof(FlutterKeyEvent);
    flutter_event.timestamp =
        static_cast<double>(LibFlutterEngine->GetCurrentTime()) / 1000.0;
    flutter_event.type = type;
    flutter_event.physical = physical;
    flutter_event.logical = logical;
    flutter_event.character =
        cb_data->character.empty() ? nullptr : cb_data->character.c_str();
    flutter_event.synthesized = false;
    flutter_event.device_type = kFlutterKeyEventDeviceTypeKeyboard;

    Engine* eng = engine_;
    asio::post(
        *eng->GetPlatformTaskRunner()->GetStrandContext(),
        [eng, flutter_event, cb_data]() mutable {
          // Re-point character pointer inside the lambda copy since the
          // original stack variable is gone; cb_data owns the string.
          flutter_event.character =
              cb_data->character.empty() ? nullptr : cb_data->character.c_str();
          eng->SendKeyEvent(flutter_event, OnKeyEventResponse, cb_data);
        });
  }

  // -----------------------------------------------------------------------
  // 2. Send via channel (flutter/keyevent) — legacy RawKeyboard path.
  //    Required for ModalRoute ESC dismissal and other Shortcuts-bridge
  //    features that rely on the RawKeyboard system.
  // -----------------------------------------------------------------------
  // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
  rapidjson::Document event(rapidjson::kObjectType);
  auto& allocator = event.GetAllocator();
  event.AddMember(kKeyCodeKey, keysym, allocator);
  event.AddMember(kKeyMapKey, kLinuxKeyMap, allocator);
  event.AddMember(kToolkitKey, kValueToolkitGtk, allocator);
  event.AddMember(kScanCodeKey, xkb_scancode, allocator);
  event.AddMember(kModifiersKey, modifiers, allocator);
  if (utf32) {
    event.AddMember(kUnicodeScalarValues, utf32, allocator);
  }
  event.AddMember(kTypeKey, released ? kKeyUp : kKeyDown, allocator);

  channel_->Send(event);
}

}  // namespace flutter
