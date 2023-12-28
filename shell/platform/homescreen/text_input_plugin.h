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

 private:
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
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_HOMESCREEN_TEXT_INPUT_PLUGIN_H
