// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_SHELL_PLATFORM_DESKTOP_KEY_EVENT_HANDLER_H
#define FLUTTER_SHELL_PLATFORM_DESKTOP_KEY_EVENT_HANDLER_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <xkbcommon/xkbcommon-names.h>
#include <xkbcommon/xkbcommon.h>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "shell/platform/homescreen/keyboard_hook_handler.h"

#include "flutter/shell/platform/common/client_wrapper/include/flutter/basic_message_channel.h"
#include "rapidjson/document.h"
#include "rapidjson/rapidjson.h"

class Engine;

namespace flutter {

class TextInputPlugin;

// Implements a KeyboardHookHandler
//
// Handles key events and forwards them to the Flutter engine.
// Key events are dispatched via both:
//   1. FlutterEngineSendKeyEvent (embedder API) for HardwareKeyboard/Focus
//      (primary path).
//   2. flutter/keyevent channel — REQUIRED, not optional: the framework's
//      KeyEventManager queues the modern KeyData from (1) and only flushes it
//      to HardwareKeyboard when a raw flutter/keyevent message arrives. It also
//      drives the legacy RawKeyboard / Shortcuts path.
//      The channel send is sequenced *after* SendKeyEvent, inside the same
//      asio post, so the two dispatches always arrive in order.
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

  // |KeyboardHookHandler|
  // Clears the pressed-key tracking table so that the next key-down after
  // focus re-entry is correctly classified as Down rather than Repeat.
  void FocusLost() override;

  // |KeyboardHookHandler|
  // Recomputes modifier bitmasks from the active XKB keymap so that custom
  // keymaps with reordered modifiers (Shift, Ctrl, Alt) are handled correctly.
  void KeymapChanged(xkb_keymap* keymap) override;

 private:
  // Non-owning pointers set after construction; nulled in the destructor so
  // that in-flight async callbacks can detect teardown via alive_.
  Engine* engine_ = nullptr;
  flutter::TextInputPlugin* text_input_ = nullptr;

  // Lifetime sentinel shared with in-flight KeyEventCallbackData instances.
  // Set to false in the destructor; callbacks check it before dereferencing
  // engine_ or text_input_ to avoid use-after-free.
  std::shared_ptr<std::atomic<bool>> alive_ =
      std::make_shared<std::atomic<bool>>(true);

  // Modifier bitmasks computed from the active XKB keymap in KeymapChanged().
  // Defaults match the standard Linux/XKB layout (Shift=0, Ctrl=2, Alt=3).
  // Atomic because KeymapChanged() writes them on the platform strand while
  // KeyboardHook() reads them on the input thread (Wayland event thread, or
  // the main thread for key-repeat) — see pressed_keys_mutex_.
  std::atomic<uint32_t> ctrl_mask_{0x4u};
  std::atomic<uint32_t> alt_mask_{0x8u};

  // Track the logical key used on key-down so that the matching key-up sends
  // the same logical key (utf32 is 0 on release, which would produce a
  // different value). Indexed by physical key code.
  //
  // KeyboardHook() may run concurrently on two threads on the Wayland backend:
  // real key events arrive on the Wayland event thread while key-repeat events
  // are driven from the main thread's EventTimer. Guard the map so the
  // read-modify-write below is not a concurrent-mutation data race.
  std::mutex pressed_keys_mutex_;
  std::unordered_map<uint64_t, uint64_t> pressed_logical_keys_;

  // In-process clipboard: persists across text-field switches so that text
  // copied from one field can be pasted into another.
  // TODO: integrate with the system clipboard
  std::string clipboard_;

  // The Flutter system channel for key event messages (flutter/keyevent).
  // ALWAYS sent — it is the framework's flush trigger for the modern
  // HardwareKeyboard path: KeyEventManager queues non-synthesized KeyData
  // (from FlutterEngineSendKeyEvent) and only dispatches it to HardwareKeyboard
  // when a raw flutter/keyevent message arrives. Gating this send out silently
  // dropped all real hardware keys.
  std::unique_ptr<flutter::BasicMessageChannel<rapidjson::Document>> channel_;
  // Reused document for flutter/keyevent channel sends. Avoids a fresh
  // allocator-chunk heap allocation per keystroke. Accessed only from the
  // platform strand.
  rapidjson::Document legacy_event_doc_;
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_DESKTOP_KEY_EVENT_HANDLER_H
