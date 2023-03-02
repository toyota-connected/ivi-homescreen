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

#pragma once

#include <map>
#include <memory>
#include <string>

#include <flutter/basic_message_channel.h>
#include <flutter/binary_messenger.h>
#include <flutter/shell/platform/common/json_message_codec.h>
#include "flutter/fml/macros.h"

#include <flutter_embedder.h>
#include <rapidjson/document.h>
#include <xkbcommon/xkbcommon.h>

#include "constants.h"

class Engine;

class DelegateHandleKey {
 public:

  typedef void (*HandleKeyHook)(void *data, FlutterKeyEventType type,
                  uint32_t xkb_scancode, xkb_keysym_t keysym);

  explicit DelegateHandleKey(void *data, HandleKeyHook hook,
      FlutterKeyEventType, uint32_t xkb_scancode, xkb_keysym_t keysym);

  /**
   * @brief Run the registered callback
   * @return void
   * @relation
   * internal
   */
  void TriggerHook();

 private:
  void* m_data;
  HandleKeyHook m_hook;
  FlutterKeyEventType m_type;
  uint32_t m_xkb_scancode;
  xkb_keysym_t m_keysym;
};

class KeyEvent : public flutter::BinaryMessenger {
 public:
  static constexpr char kChannelName[] = "flutter/keyevent";

  explicit KeyEvent();

  /**
   * @brief Callback function for platform messages about keyevent
   * @param[in] message Received message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   *
   * When a key event is sent through "flutter/keyevent",
   * any platform messages seems not to be received.
   * So, this callback is defined, but, not registered.
   */
  MAYBE_UNUSED static void OnPlatformMessage(
      const FlutterPlatformMessage* message,
      void* userdata);

  /**
   * @brief Set the flutter engine for key event
   * @param[in] engine Pointer to Flutter engine
   * @return void
   * @relation
   * flutter
   */
  void SetEngine(const std::shared_ptr<Engine>& engine);

  /**
   * @brief the handler for key event
   * @param[in] data Pointer to Ueer data
   * @param[in] type the type of Flutter KeyEvent
   * @param[in] code key code
   * @param[in] sym key symbol
   * @param[in] hook_data Pointer to Hook data
   * @paran[in] hook another callback when a key is handled.
   *            This will be called only when a key event is not handled in flutter app.
   * @return void
   * @relation
   * flutter
   *
   * TODO: support modifiers
   * WARNING:
   *   this api's interface is not stable.
   *   After supporting modifiers, the interface will be changed.
   */
  static void keyboard_handle_key(void* data,
                                  FlutterKeyEventType eventType,
                                  uint32_t xkb_scancode,
                                  xkb_keysym_t keysym,
                                  std::shared_ptr<DelegateHandleKey> delegate);

  FML_DISALLOW_COPY_AND_ASSIGN(KeyEvent);

 private:
  static constexpr char kKeyCodeKey[] = "keyCode";
  static constexpr char kKeyMapKey[] = "keymap";
  static constexpr char kScanCodeKey[] = "scanCode";
  static constexpr char kModifiersKey[] = "modifiers";
  static constexpr char kTypeKey[] = "type";
  static constexpr char kToolkitKey[] = "toolkit";
  static constexpr char kUnicodeScalarValues[] = "unicodeScalarValues";
  static constexpr char kLinuxKeyMap[] = "linux";
  static constexpr char kKeyUp[] = "keyup";
  static constexpr char kKeyDown[] = "keydown";
  static constexpr char kValueToolkitGtk[] = "gtk";
  static constexpr char kHandled[] = "handled";

  std::shared_ptr<Engine> engine_;

  std::unique_ptr<flutter::BasicMessageChannel<rapidjson::Document>> channel_;

  /**
   * @brief convert key event from xkb to flutter and send it to flutter engine
   * @param[in] released true if a key is released and vice versa
   * @param[in] sym key symbol
   * @param[in] code key code
   * @param[in] modifiers key modifiers
   * @return void
   * @relation
   * flutter, wayland
   */
  void SendKeyEvent(bool released,
                    xkb_keysym_t keysym,
                    uint32_t xkb_scancode,
                    uint32_t modifiers,
                    std::shared_ptr<DelegateHandleKey> delegate);

  mutable flutter::BinaryReply last_binary_reply_;
  std::string last_message_handler_channel_;
  flutter::BinaryMessageHandler last_message_handler_;

  /**
   * @brief The reply callback impl for SendPlatformMessage
   * @param[in] the reply data
   * @param[in] the reply size
   * @param[in] user data
   * @return void
   * @relation
   * flutter
   */
  static void ReplyHandler(const uint8_t *data, size_t data_size,
                           void* userdata) {
    auto d = reinterpret_cast<KeyEvent*>(userdata);
    if (d->last_binary_reply_) {
      d->last_binary_reply_(data, data_size);
    }
  }

  typedef enum {
    FL_KEY_EV_RET_IGNORED = 0,
    FL_KEY_EV_RET_HANDLED,
    FL_KEY_EV_RET_FAILED,
  } FL_KEY_EV_RET_T;

  /**
   * @brief parse a reply from a flutter app
   * @param[in] the reply data
   * @param[in] the reply size
   * @return FL_KEY_EV_RET_T
   *         If it is failed to parse a reply, return FL_KEY_EV_RET_FAILED.
   *         If it is complete to parse a reply, return FL_KEY_EV_RET_HANDLED or FL_KEY_EV_RET_IGNORED.
   *         FL_KEY_EV_RET_IGNORED is corresponding to KeyEventResult.ignored.
   *         FL_KEY_EV_RET_HANDLED is corresponding to KeyEventResult.handled.
   * @relation
   * flutter
   */
  static FL_KEY_EV_RET_T ParseReply(const uint8_t* reply, size_t reply_size);


  /**
   * @brief Send a message to the Flutter engine on this channel
   * @param[in] channel This channel
   * @param[in] message a message
   * @param[in] message_size the size of a message
   * @param[in] reply Binary message reply callback
   * @return void
   * @relation
   * flutter
   */
  void Send(const std::string& channel,
            const uint8_t* message,
            size_t message_size,
            flutter::BinaryReply reply) const override;

  /**
   * @brief Registers a handler to be invoked when the Flutter application sends
   * a message to its host platform
   * @param[in] channel This channel
   * @param[in] handler a handler for incoming binary messages from Flutter
   * @return void
   * @relation
   * flutter
   */
  MAYBE_UNUSED NODISCARD void SetMessageHandler(
      const std::string& channel,
      flutter::BinaryMessageHandler handler) override;
};
