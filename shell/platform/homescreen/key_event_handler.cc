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
  uint32_t ctrl_mask;  // cached from KeyEventHandler at dispatch time
  uint32_t alt_mask;   // cached from KeyEventHandler at dispatch time
  bool released;
  // Weak reference to the owning handler's lifetime sentinel.
  // If the handler has been destroyed, the locked shared_ptr will be null or
  // the atomic will be false, and the callback must not touch engine/plugin.
  std::weak_ptr<std::atomic<bool>> handler_alive;
  // Platform strand used to post the TextInputPlugin fallback call so
  // that all TextInputPlugin state mutations are serialised on one thread.
  asio::io_context::strand* strand;
};

// Converts a UTF-32 codepoint to a UTF-8 string (for
// FlutterKeyEvent.character).
// M2: lone surrogates [0xD800, 0xDFFF] and values above U+10FFFF are
// explicitly rejected (RFC 3629 §3).  Returns an empty string for those.
std::string Utf32ToUtf8(const uint32_t codepoint) {
  // Reject lone surrogates and out-of-range values.
  if ((codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF) {
    return {};
  }
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
  } else {
    out += static_cast<char>(0xf0 | (codepoint >> 18));
    out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (codepoint & 0x3f));
  }
  return out;
}

// Callback invoked by the Flutter engine after processing a key event via the
// embedder API. If the event was not handled, fall back to TextInputPlugin so
// that special keys (backspace, arrows, home/end, etc.) and unhandled printable
// characters are still handled by the text input model.
void OnKeyEventResponse(bool handled, void* user_data) {
  auto* data = static_cast<KeyEventCallbackData*>(user_data);

  // Guard against use-after-free: if the owning KeyEventHandler has been
  // destroyed the sentinel will be false (or the weak_ptr expired).
  const auto alive = data->handler_alive.lock();
  if (!alive || !alive->load()) {
    delete data;
    return;
  }

  if (!handled && data->text_input != nullptr) {
    // Log when a control or alt modifier is active alongside a printable
    // character — this is usually a keyboard shortcut (e.g. Ctrl+C).
    // TextInputPlugin::KeyboardHook handles the suppression of such keys
    // for normal text insertion while still allowing the unicode input state
    // machine (Ctrl+Shift+U activation and in-mode hex digits) to work.
    const bool ctrl_or_alt = (data->modifiers & data->ctrl_mask) != 0 ||
                             (data->modifiers & data->alt_mask) != 0;
    if (ctrl_or_alt && !data->character.empty()) {
      SPDLOG_DEBUG(
          "[key] ctrl/alt + char key forwarded to TextInputPlugin "
          "(mods=0x{:02x})",
          data->modifiers);
    }
    // Post the fallback onto the platform strand so that TextInputPlugin
    // state is only ever mutated from the strand, preventing concurrent access
    // with the clipboard path and FocusLost/KeymapChanged callbacks.
    auto* text_input = data->text_input;
    const auto released = data->released;
    const auto keysym = data->keysym;
    const auto xkb_scancode = data->xkb_scancode;
    const auto modifiers = data->modifiers;
    asio::post(*data->strand, [text_input, released, keysym, xkb_scancode,
                               modifiers]() {
      text_input->KeyboardHook(released, keysym, xkb_scancode, modifiers);
    });
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

KeyEventHandler::~KeyEventHandler() {
  // Signal all in-flight callbacks that this handler is gone.
  // After this any pending OnKeyEventResponse / lambda will bail early
  // and free cb_data without touching engine_ or text_input_.
  alive_->store(false);
  engine_ = nullptr;
  text_input_ = nullptr;
  // M1: explicitly zero the clipboard so sensitive text (e.g. passwords)
  // does not linger in the process heap after the handler is torn down.
  // std::string's destructor does not zero its buffer.
  if (!clipboard_.empty()) {
    std::fill(clipboard_.begin(), clipboard_.end(), '\0');
    clipboard_.clear();
  }
  // NOTE: Do not change the teardown order so that the strand is destroyed
  // before this handler.
}

void KeyEventHandler::CharHook(unsigned int /* code_point */) {}

void KeyEventHandler::FocusLost() {
  // NOTE: Do NOT clear pressed_logical_keys_ here.
  // Doing so results in issues when keystrokes are repeated on focus re-entry.

  // Forward to TextInputPlugin
  if (text_input_ != nullptr) {
    text_input_->FocusLost();
  }
}

void KeyEventHandler::KeymapChanged(xkb_keymap* keymap) {
  const xkb_mod_index_t ctrl_idx =
      xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
  if (ctrl_idx != XKB_MOD_INVALID) {
    ctrl_mask_ = 1u << ctrl_idx;
  }
  const xkb_mod_index_t alt_idx =
      xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);
  if (alt_idx != XKB_MOD_INVALID) {
    alt_mask_ = 1u << alt_idx;
  }
}

void KeyEventHandler::KeyboardHook(const bool released,
                                   const xkb_keysym_t keysym,
                                   const uint32_t xkb_scancode,
                                   const uint32_t modifiers) {
  const uint64_t physical = key_mapping::XkbScancodeToPhysicalKey(xkb_scancode);
  const uint32_t utf32 = xkb_keysym_to_utf32(keysym);

  // SPDLOG_DEBUG(
  //     "[key] {} keysym=0x{:08x} scan={} mods=0x{:02x} "
  //     "physical=0x{:016x} utf32=0x{:04x}",
  //     released ? "up  " : "down", keysym, xkb_scancode, modifiers, physical,
  //     utf32);

  // Determine event type and logical key, tracking down/up pairs so that
  // key-up always sends the same logical key as the corresponding key-down
  // (utf32 is 0 on release which would otherwise yield a different value).
  FlutterKeyEventType type;
  uint64_t logical;

  if (!released) {
    const bool already_pressed = pressed_logical_keys_.count(physical) > 0;
    type =
        already_pressed ? kFlutterKeyEventTypeRepeat : kFlutterKeyEventTypeDown;
    logical = key_mapping::KeysymToLogicalKey(keysym, utf32, physical);
    pressed_logical_keys_[physical] = logical;
  } else {
    type = kFlutterKeyEventTypeUp;
    const auto it = pressed_logical_keys_.find(physical);
    logical = (it != pressed_logical_keys_.end())
                  ? it->second
                  : key_mapping::KeysymToLogicalKey(keysym, utf32, physical);
    pressed_logical_keys_.erase(physical);

    // If this is a key release for a key we haven't seen pressed,
    // do not send the event.
    //
    // Eg. occurs when Alt+Tab'ing into the app
    if (it == pressed_logical_keys_.end()) {
      spdlog::debug(
          "[key] ignoring key-up for unpressed key "
          "(keysym=0x{:08x} scan={} mods=0x{:02x} physical=0x{:016x})",
          keysym, xkb_scancode, modifiers, physical);
      return;
    }
  }

  if (engine_ != nullptr) {
    // Build character string (only for key-down/repeat of printable chars).
    std::string character_str;
    if (!released && utf32 >= 0x20 && utf32 != 0x7f) {
      character_str = Utf32ToUtf8(utf32);
    }

    // Heap-allocate event data — ownership passes to OnKeyEventResponse.
    auto* cb_data = new KeyEventCallbackData{
        text_input_, character_str,
        keysym,      xkb_scancode,
        modifiers,   ctrl_mask_,
        alt_mask_,   released,
        alive_,      engine_->GetPlatformTaskRunner()->GetStrandContext()};

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

    // SPDLOG_DEBUG(
    //     "[key] -> embedder type={} physical=0x{:016x} "
    //     "logical=0x{:016x} char='{}'",
    //     static_cast<int>(type), physical, logical,
    //     character_str.empty() ? "(none)" : character_str);

    Engine* eng = engine_;
    asio::post(
        *eng->GetPlatformTaskRunner()->GetStrandContext(),
        [eng, flutter_event, cb_data, utf32, keysym, xkb_scancode, modifiers,
         released, channelPtr = channel_.get(), clipboard = &clipboard_,
         weak_alive = std::weak_ptr<std::atomic<bool>>(alive_)]() mutable {
          // Bail if the owning KeyEventHandler was destroyed while this
          // post was queued; free cb_data to prevent the leak.
          const auto alive = weak_alive.lock();
          if (!alive || !alive->load()) {
            delete cb_data;
            return;
          }

          // Handle clipboard shortcuts (Ctrl+C, Ctrl+X, Ctrl+V) on key-down
          // before dispatching to the Flutter engine.  The clipboard outlives
          // individual text fields so copy/paste works across them.
          if (!released && cb_data->text_input != nullptr &&
              !cb_data->text_input->IsUnicodeInputActive()) {
            const bool ctrl_active = (modifiers & cb_data->ctrl_mask) != 0;
            if (ctrl_active) {
              // Skip clipboard shortcuts while Unicode input mode is active —
              // pasting mid-composition corrupts the composing region's
              // backspace count.
              if (keysym == XKB_KEY_c || keysym == XKB_KEY_C) {
                // Copy: save selected text to the in-process clipboard.
                *clipboard = cb_data->text_input->GetSelectedText();
              } else if (keysym == XKB_KEY_x || keysym == XKB_KEY_X) {
                // Cut: copy then delete the selection.
                *clipboard = cb_data->text_input->GetSelectedText();
                if (!clipboard->empty()) {
                  cb_data->text_input->DeleteSelectedText();
                }
              } else if (keysym == XKB_KEY_v || keysym == XKB_KEY_V) {
                // Paste: insert clipboard text at the cursor / over selection.
                if (!clipboard->empty()) {
                  cb_data->text_input->PasteText(*clipboard);
                }
              }
            }
          }

          // cb_data is heap-allocated; character pointer is valid.
          flutter_event.character =
              cb_data->character.empty() ? nullptr : cb_data->character.c_str();

          // 1. Send via embedder API
          // `FlutterEngineSendKeyEvent`/`HardwareKeyboard`
          //    (primary path)
          const auto rc =
              eng->SendKeyEvent(flutter_event, OnKeyEventResponse, cb_data);

          // Drive the fallback path on failure
          if (rc != kSuccess) {
            OnKeyEventResponse(false, cb_data);
          }

          // 2. Send via channel `flutter/keyevent`
          //    (legacy RawKeyboard path, deprecated from Flutter 3.19)
          //    Sent after SendKeyEvent to ensure correct ordering.
          // NOLINTNEXTLINE(clang-analyzer-core.NullDereference)
          rapidjson::Document event(rapidjson::kObjectType);
          auto& allocator = event.GetAllocator();
          // M4: keysym is uint32_t; some Unicode-plane values exceed INT32_MAX
          // and would be sign-extended to negative by the int overload.  Use
          // uint64_t so the JSON recipient always sees a non-negative number.
          event.AddMember(kKeyCodeKey, static_cast<uint64_t>(keysym),
                          allocator);
          event.AddMember(kKeyMapKey, kLinuxKeyMap, allocator);
          event.AddMember(kToolkitKey, kValueToolkitGtk, allocator);
          event.AddMember(kScanCodeKey, xkb_scancode, allocator);
          event.AddMember(kModifiersKey, modifiers, allocator);
          if (utf32) {
            event.AddMember(kUnicodeScalarValues, utf32, allocator);
          }
          event.AddMember(kTypeKey,
                          rapidjson::StringRef(released ? kKeyUp : kKeyDown),
                          allocator);

          channelPtr->Send(event);
        });
  }
}

}  // namespace flutter
