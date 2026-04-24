// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_DESKTOP_KEY_EVENT_HANDLER_H
#define FLUTTER_SHELL_PLATFORM_DESKTOP_KEY_EVENT_HANDLER_H

#include <memory>
#include <unordered_map>

#include "rapidjson/rapidjson.h"

#include "flutter/shell/platform/common/client_wrapper/include/flutter/basic_message_channel.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "rapidjson/document.h"
#include "shell/platform/homescreen/keyboard_hook_handler.h"

class Engine;

namespace flutter {

class TextInputPlugin;

// Implements a KeyboardHookHandler
//
// Handles key events and forwards them to the Flutter engine.
// Key events are dispatched via both:
//   1. FlutterEngineSendKeyEvent (embedder API) for HardwareKeyboard/Focus
//   2. flutter/keyevent channel (legacy RawKeyboard / Shortcuts)
class KeyEventHandler final : public KeyboardHookHandler {
 public:
  explicit KeyEventHandler(flutter::BinaryMessenger* messenger);

  ~KeyEventHandler() override;

  // Sets the engine used to dispatch key events via the embedder API.
  // Must be called after the engine is running.
  void SetEngine(Engine* engine) { engine_ = engine; }

  // Sets the TextInputPlugin used as a fallback delegate for printable
  // characters that are not handled by the Flutter framework.
  void SetTextInputPlugin(flutter::TextInputPlugin* text_input) {
    text_input_ = text_input;
  }

  // |KeyboardHookHandler|
  void KeyboardHook(bool released,
                    xkb_keysym_t keysym,
                    uint32_t xkb_scancode,
                    uint32_t modifiers) override;

  // |KeyboardHookHandler|
  void CharHook(unsigned int code_point) override;

 private:
  // The Flutter system channel for key event messages (legacy RawKeyboard).
  std::unique_ptr<flutter::BasicMessageChannel<rapidjson::Document>> channel_;

  // Non-owning pointers set after construction.
  Engine* engine_ = nullptr;
  flutter::TextInputPlugin* text_input_ = nullptr;

  // Track the logical key used on key-down so that the matching key-up sends
  // the same logical key (utf32 is 0 on release, which would produce a
  // different value). Indexed by physical key code.
  mutable std::unordered_map<uint64_t, uint64_t> pressed_logical_keys_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_DESKTOP_KEY_EVENT_HANDLER_H
