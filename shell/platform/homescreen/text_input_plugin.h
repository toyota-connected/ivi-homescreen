// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_HOMESCREEN_TEXT_INPUT_PLUGIN_H
#define FLUTTER_SHELL_PLATFORM_HOMESCREEN_TEXT_INPUT_PLUGIN_H

#include <memory>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/method_channel.h"
#include "flutter/shell/platform/common/text_input_model.h"
#include "shell/platform/homescreen/keyboard_hook_handler.h"

#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

#include "rapidjson/rapidjson.h"

#include "rapidjson/document.h"

namespace flutter {

// Implements a text input plugin.
//
// Specifically handles window events within GLFW.
class TextInputPlugin final : public KeyboardHookHandler {
 public:
  explicit TextInputPlugin(flutter::BinaryMessenger* messenger);

  ~TextInputPlugin() override;

  // |KeyboardHookHandler|
  void KeyboardHook(bool released,
                    xkb_keysym_t keysym,
                    uint32_t xkb_scancode,
                    uint32_t modifiers) override;

  // |KeyboardHookHandler|
  void CharHook(unsigned int code_point) override;

  // |KeyboardHookHandler|
  // Updates the shift modifier bitmask from the active XKB keymap so that
  // shift detection works even with custom keymaps that reorder modifiers.
  void KeymapChanged(xkb_keymap* keymap) override;

  // |KeyboardHookHandler|
  // Cancels any in-progress Unicode input composing state when focus is lost.
  void FocusLost() override;

  // Returns true while the plugin is waiting for hex digits after a
  // Ctrl+Shift+U activation (GTK-style Unicode input mode).
  bool IsUnicodeInputActive() const {
    return unicode_state_ == UnicodeInputState::kPendingHex;
  }

 private:
  // State for GTK-style Ctrl+Shift+U Unicode input.
  enum class UnicodeInputState { kNormal, kPendingHex };

  // Enters Unicode input mode: inserts 'u' into the model with a composing
  // range so Flutter renders it underlined.
  void ActivateUnicodeInput();

  // Commits the accumulated hex digits as a single Unicode codepoint, removes
  // the composing 'u' + hex text, and exits Unicode input mode.
  void CommitUnicodeInput();

  // Removes the composing 'u' + any accumulated hex digits from the model and
  // exits Unicode input mode without inserting any character.
  void CancelUnicodeInput();

  // Sends the current state of the given model to the Flutter engine.
  void SendStateUpdate(const TextInputModel& model) const;

  // Sends an action triggered by the Enter key to the Flutter engine.
  void EnterPressed(TextInputModel* model) const;

  // Called when a method is called on |channel_|;
  void HandleMethodCall(
      const flutter::MethodCall<rapidjson::Document>& method_call,
      const std::unique_ptr<flutter::MethodResult<rapidjson::Document>>&
          result);

  // The MethodChannel used for communication with the Flutter engine.
  std::unique_ptr<flutter::MethodChannel<rapidjson::Document>> channel_;

  // Bitmask for the Shift modifier; computed from the active XKB keymap in
  // KeymapChanged() so custom keymaps with reordered modifiers are handled.
  uint32_t shift_mask_ = 0x1u;

  // Bitmask for the Ctrl modifier; computed from the active XKB keymap in
  // KeymapChanged() so custom keymaps with reordered modifiers are handled.
  uint32_t ctrl_mask_ = 0x4u;

  // The active client id.
  int client_id_ = 0;

  // The active model. nullptr if not set.
  std::unique_ptr<TextInputModel> active_model_;

  // Keyboard type of the client. See available options:
  // https://api.flutter.dev/flutter/services/TextInputType-class.html
  std::string input_type_;

  // An action requested by the user on the input client. See available options:
  // https://api.flutter.dev/flutter/services/TextInputAction-class.html
  std::string input_action_;

  // GTK-style Ctrl+Shift+U Unicode input state.
  UnicodeInputState unicode_state_ = UnicodeInputState::kNormal;

  // Accumulated lowercase hex digits entered after Ctrl+Shift+U.
  std::string unicode_hex_;

  // UTF-16 code-unit indices of the composing range used to underline the
  // 'u' + hex digits while in kPendingHex state. -1 when inactive.
  int compose_base_utf16_ = -1;
  int compose_extent_utf16_ = -1;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_HOMESCREEN_TEXT_INPUT_PLUGIN_H
