// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/platform/homescreen/key_event_handler.h"

#include "flutter/shell/platform/common/json_message_codec.h"
#include "view/flutter_view.h"

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

namespace flutter {

KeyEventHandler::KeyEventHandler(flutter::BinaryMessenger* messenger)
    : channel_(
          std::make_unique<flutter::BasicMessageChannel<rapidjson::Document>>(
              messenger,
              kChannelName,
              &flutter::JsonMessageCodec::GetInstance())) {}

KeyEventHandler::~KeyEventHandler() = default;

void KeyEventHandler::CharHook(unsigned int /* code_point */) {}

void KeyEventHandler::KeyboardHook(bool released,
                                   xkb_keysym_t keysym,
                                   uint32_t xkb_scancode,
                                   uint32_t modifiers) {
  // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
  rapidjson::Document event(rapidjson::kObjectType);
  auto& allocator = event.GetAllocator();
  event.AddMember(kKeyCodeKey, keysym, allocator);
  event.AddMember(kKeyMapKey, kLinuxKeyMap, allocator);
  event.AddMember(kToolkitKey, kValueToolkitGtk, allocator);
  event.AddMember(kScanCodeKey, xkb_scancode, allocator);
  event.AddMember(kModifiersKey, modifiers, allocator);

  const uint32_t utf32_code = xkb_keysym_to_utf32(keysym);
  if (utf32_code)
    event.AddMember(kUnicodeScalarValues, utf32_code, allocator);
  if (released) {
    event.AddMember(kTypeKey, kKeyUp, allocator);
  } else {
    event.AddMember(kTypeKey, kKeyDown, allocator);
  }

  channel_->Send(event);
}
}  // namespace flutter
