/*
 * Copyright 2020-2022 Toyota Connected North America
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

#pragma once

#include <map>
#include <memory>
#include <string>

#include <flutter/binary_messenger.h>
#include <flutter/method_channel.h>
#include <flutter/method_result_functions.h>
#include "flutter/fml/macros.h"
#include "flutter/shell/platform/common/json_method_codec.h"
#include "flutter/shell/platform/common/text_input_model.h"

#include <flutter_embedder.h>
#include <rapidjson/document.h>
#include <xkbcommon/xkbcommon.h>

class App;
class Engine;

class TextInput : public flutter::BinaryMessenger {
 public:
  static constexpr char kChannelName[] = "flutter/textinput";

  explicit TextInput();

  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);

  void SetEngine(const std::shared_ptr<Engine>& engine);

  static void keyboard_handle_key(void* data,
                                  struct wl_keyboard* keyboard,
                                  uint32_t serial,
                                  uint32_t time,
                                  xkb_keysym_t keysym,
                                  uint32_t state);

  FML_DISALLOW_COPY_AND_ASSIGN(TextInput);

 private:
  static constexpr char kSetEditingStateMethod[] = "TextInput.setEditingState";
  static constexpr char kClearClientMethod[] = "TextInput.clearClient";
  static constexpr char kSetClientMethod[] = "TextInput.setClient";
  static constexpr char kShowMethod[] = "TextInput.show";
  static constexpr char kHideMethod[] = "TextInput.hide";

  static constexpr char kMultilineInputType[] = "TextInputType.multiline";

  static constexpr char kUpdateEditingStateMethod[] =
      "TextInputClient.updateEditingState";
  static constexpr char kPerformActionMethod[] =
      "TextInputClient.performAction";

  static constexpr char kTextInputAction[] = "inputAction";
  static constexpr char kTextInputType[] = "inputType";
  static constexpr char kTextInputTypeName[] = "name";
  static constexpr char kComposingBaseKey[] = "composingBase";
  static constexpr char kComposingExtentKey[] = "composingExtent";
  static constexpr char kSelectionAffinityKey[] = "selectionAffinity";
  static constexpr char kAffinityDownstream[] = "TextAffinity.downstream";
  static constexpr char kSelectionBaseKey[] = "selectionBase";
  static constexpr char kSelectionExtentKey[] = "selectionExtent";
  static constexpr char kSelectionIsDirectionalKey[] = "selectionIsDirectional";
  static constexpr char kTextKey[] = "text";

  static constexpr char kBadArgumentError[] = "Bad Arguments";
  static constexpr char kInternalConsistencyError[] =
      "Internal Consistency Error";

  // Sends the current state of the given model to the Flutter engine.
  void SendStateUpdate(const flutter::TextInputModel& model);

  // Sends an action triggered by the Enter key to the Flutter engine.
  void EnterPressed(flutter::TextInputModel* model);

  // The MethodChannel used for communication with the Flutter engine.
  std::unique_ptr<flutter::MethodChannel<rapidjson::Document>> channel_;

  // The active client id.
  int client_id_;

  // The active model. nullptr if not set.
  std::unique_ptr<flutter::TextInputModel> active_model_;

  // Keyboard type of the client. See available options:
  // https://docs.flutter.io/flutter/services/TextInputType-class.html
  std::string input_type_;

  // An action requested by the user on the input client. See available options:
  // https://docs.flutter.io/flutter/services/TextInputAction-class.html
  std::string input_action_;

  std::shared_ptr<Engine> engine_;

  mutable flutter::BinaryReply last_reply_handler_;
  std::string last_message_handler_channel_;
  flutter::BinaryMessageHandler last_message_handler_;

  void Send(const std::string& channel,
            const uint8_t* message,
            size_t message_size,
            flutter::BinaryReply reply) const override;

  void SetMessageHandler(const std::string& channel,
                         flutter::BinaryMessageHandler handler) override;
};
