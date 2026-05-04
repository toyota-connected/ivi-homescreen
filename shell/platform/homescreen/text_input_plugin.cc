// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/platform/homescreen/text_input_plugin.h"

#include <algorithm>

#include "flutter/shell/platform/common/json_method_codec.h"

static constexpr char kSetEditingStateMethod[] = "TextInput.setEditingState";
static constexpr char kClearClientMethod[] = "TextInput.clearClient";
static constexpr char kSetClientMethod[] = "TextInput.setClient";
static constexpr char kShowMethod[] = "TextInput.show";
static constexpr char kHideMethod[] = "TextInput.hide";

static constexpr char kMultilineInputType[] = "TextInputType.multiline";

static constexpr char kUpdateEditingStateMethod[] =
    "TextInputClient.updateEditingState";
static constexpr char kPerformActionMethod[] = "TextInputClient.performAction";

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

static constexpr char kChannelName[] = "flutter/textinput";

static constexpr char kBadArgumentError[] = "Bad Arguments";
static constexpr char kInternalConsistencyError[] =
    "Internal Consistency Error";

namespace flutter {

// Converts a UTF-8 byte offset into the corresponding UTF-16 code-unit index.
// A BMP character (1–3 UTF-8 bytes) occupies 1 UTF-16 unit; a supplementary
// character (4 UTF-8 bytes) occupies 2 UTF-16 units (surrogate pair).
// |utf8_pos| must not land in the middle of a multi-byte sequence; it is
// clamped to the end of the string if it exceeds it.
static size_t Utf8PosToUtf16Index(const std::string& utf8, size_t utf8_pos) {
  utf8_pos = std::min(utf8_pos, utf8.size());
  size_t u16 = 0;
  size_t i = 0;
  while (i < utf8_pos) {
    const auto c = static_cast<unsigned char>(utf8[i]);
    size_t seq_len;
    if (c < 0x80) {
      seq_len = 1;
    } else if (c < 0xe0) {
      seq_len = 2;
    } else if (c < 0xf0) {
      seq_len = 3;
    } else {
      seq_len = 4;  // supplementary plane → surrogate pair → 2 UTF-16 units
    }
    u16 += (seq_len == 4) ? 2u : 1u;
    i += seq_len;
  }
  return u16;
}

// Returns the byte offset that is |col| columns into the line starting at
// |line_start| in |text|, clamped to the line end (the '\n' or end of string).
static size_t ClampedColumn(const std::string& text,
                            size_t line_start,
                            size_t col) {
  const size_t line_end = text.find('\n', line_start);
  const size_t line_len = (line_end == std::string::npos)
                              ? text.size() - line_start
                              : line_end - line_start;

  return line_start + std::min(col, line_len);
}

// Returns the cursor position for moving up one line, preserving column.
// Returns |pos| unchanged when already on the first line (no-op).
static size_t PreviousLinePosition(const std::string& text, size_t pos) {
  // Find the '\n' that ends the previous line.
  const size_t prev_nl =
      (pos == 0) ? std::string::npos : text.rfind('\n', pos - 1);
  if (prev_nl == std::string::npos) {
    // first line: return beginning of string
    return 0;
  }
  // Start of current line (one past the '\n' we found).
  const size_t cur_line_start = prev_nl + 1;
  const size_t col = pos - cur_line_start;

  // Find start of the previous line.
  const size_t prev_line_start = (prev_nl == 0) ? 0 : [&] {
    const size_t pp = text.rfind('\n', prev_nl - 1);
    return (pp == std::string::npos) ? 0 : pp + 1;
  }();

  return ClampedColumn(text, prev_line_start, col);
}

// Returns the cursor position for moving down one line, preserving column.
// Returns |pos| unchanged when already on the last line (no-op).
static size_t NextLinePosition(const std::string& text, size_t pos) {
  const size_t cur_nl = text.find('\n', pos);
  if (cur_nl == std::string::npos) {
    // last line: return end of string
    return text.size();
  }
  // Start of current line for column calculation.
  const size_t cur_line_start = [&] {
    if (pos == 0)
      return size_t{0};
    const size_t pp = text.rfind('\n', pos - 1);
    return (pp == std::string::npos) ? 0 : pp + 1;
  }();
  const size_t col = pos - cur_line_start;
  const size_t next_line_start = cur_nl + 1;

  return ClampedColumn(text, next_line_start, col);
}

void TextInputPlugin::CharHook(const unsigned int code_point) {
  // SPDLOG_DEBUG("TextInputPlugin::CharHook: code_point: {}", code_point);
  if (active_model_ == nullptr) {
    return;
  }
  active_model_->AddCodePoint(code_point);
  SendStateUpdate(*active_model_);
}

void TextInputPlugin::KeyboardHook(bool released,
                                   xkb_keysym_t keysym,
                                   uint32_t /* xkb_scancode */,
                                   uint32_t modifiers) {
  if (active_model_ == nullptr) {
    return;
  }
  if (!released) {
    const bool shift = (modifiers & shift_mask_) != 0;
    switch (keysym) {
      case XKB_KEY_BackSpace:
        if (active_model_->Backspace()) {
          SendStateUpdate(*active_model_);
        }
        break;
      // Arrow selection and movement keys. See:
      // https://api.flutter.dev/flutter/services/LogicalKeyboardKey-class.html
      case XKB_KEY_Left:
      case XKB_KEY_KP_Left:
        if (shift) {
          // Extend/shrink the selection by one code point toward the start.
          // Use the model's own MoveCursorBack so surrogate pairs are handled
          // correctly (advances 2 UTF-16 units for supplementary characters).
          const size_t old_base = active_model_->selection().base();
          active_model_->SetSelection(
              TextRange(active_model_->selection().extent()));
          active_model_->MoveCursorBack();
          active_model_->SetSelection(
              TextRange(old_base, active_model_->selection().position()));
          SendStateUpdate(*active_model_);
        } else if (active_model_->MoveCursorBack()) {
          SendStateUpdate(*active_model_);
        }
        break;
      case XKB_KEY_Right:
      case XKB_KEY_KP_Right:
        if (shift) {
          // Extend/shrink the selection by one code point toward the end.
          const size_t old_base = active_model_->selection().base();
          active_model_->SetSelection(
              TextRange(active_model_->selection().extent()));
          active_model_->MoveCursorForward();
          active_model_->SetSelection(
              TextRange(old_base, active_model_->selection().position()));
          SendStateUpdate(*active_model_);
        } else if (active_model_->MoveCursorForward()) {
          SendStateUpdate(*active_model_);
        }
        break;
      case XKB_KEY_Up:
      case XKB_KEY_KP_Up: {
        // GetCursorOffset() returns the UTF-8 byte offset of
        // selection.extent(). PreviousLinePosition works in UTF-8 byte space.
        // Convert the result back to a UTF-16 code-unit index for SetSelection.
        const std::string text = active_model_->GetText();
        const auto utf8_pos =
            static_cast<size_t>(active_model_->GetCursorOffset());
        const size_t new_utf8_pos = PreviousLinePosition(text, utf8_pos);
        const size_t new_u16_pos = Utf8PosToUtf16Index(text, new_utf8_pos);
        if (new_u16_pos != active_model_->selection().extent()) {
          active_model_->SetSelection(
              shift ? TextRange(active_model_->selection().base(), new_u16_pos)
                    : TextRange(new_u16_pos));
          SendStateUpdate(*active_model_);
        }
        break;
      }
      case XKB_KEY_Down:
      case XKB_KEY_KP_Down: {
        const std::string text = active_model_->GetText();
        const auto utf8_pos =
            static_cast<size_t>(active_model_->GetCursorOffset());
        const size_t new_utf8_pos = NextLinePosition(text, utf8_pos);
        const size_t new_u16_pos = Utf8PosToUtf16Index(text, new_utf8_pos);
        if (new_u16_pos != active_model_->selection().extent()) {
          active_model_->SetSelection(
              shift ? TextRange(active_model_->selection().base(), new_u16_pos)
                    : TextRange(new_u16_pos));
          SendStateUpdate(*active_model_);
        }
        break;
      }
      case XKB_KEY_End:
      case XKB_KEY_KP_End:
        if (shift) {
          active_model_->SelectToEnd();
        } else {
          active_model_->MoveCursorToEnd();
        }
        SendStateUpdate(*active_model_);
        break;
      case XKB_KEY_Home:
      case XKB_KEY_KP_Home:
        if (shift) {
          active_model_->SelectToBeginning();
        } else {
          active_model_->MoveCursorToBeginning();
        }
        SendStateUpdate(*active_model_);
        break;
      case XKB_KEY_Delete:
      case XKB_KEY_KP_Delete:
        if (active_model_->Delete()) {
          SendStateUpdate(*active_model_);
        }
        break;
      case XKB_KEY_Return:
      case XKB_KEY_ISO_Enter:
      case XKB_KEY_KP_Enter:
        EnterPressed(active_model_.get());
        break;
      case XKB_KEY_Shift_L:
      case XKB_KEY_Shift_R:
      case XKB_KEY_Control_L:
      case XKB_KEY_Control_R:
      case XKB_KEY_Caps_Lock:
      case XKB_KEY_Shift_Lock:
      case XKB_KEY_Meta_L:
      case XKB_KEY_Meta_R:
      case XKB_KEY_Alt_L:
      case XKB_KEY_Alt_R:
      case XKB_KEY_Super_L:
      case XKB_KEY_Super_R:
      case XKB_KEY_Hyper_L:
      case XKB_KEY_Hyper_R:
      case XKB_KEY_Tab:
      case XKB_KEY_Linefeed:
      case XKB_KEY_Clear:
      case XKB_KEY_Pause:
      case XKB_KEY_Scroll_Lock:
      case XKB_KEY_Sys_Req:
      case XKB_KEY_Escape:
      case XKB_KEY_Page_Up:
      case XKB_KEY_Page_Down:
      case XKB_KEY_Begin:
      case XKB_KEY_Print:
      case XKB_KEY_Insert:
      case XKB_KEY_Menu:
      case XKB_KEY_Num_Lock:
      case XKB_KEY_KP_Page_Up:
      case XKB_KEY_KP_Page_Down:
      case XKB_KEY_KP_Begin:
      case XKB_KEY_KP_Insert:
      case XKB_KEY_KP_Tab:
      case XKB_KEY_F1:
      case XKB_KEY_F2:
      case XKB_KEY_F3:
      case XKB_KEY_F4:
      case XKB_KEY_F5:
      case XKB_KEY_F6:
      case XKB_KEY_F7:
      case XKB_KEY_F8:
      case XKB_KEY_F9:
      case XKB_KEY_F10:
      case XKB_KEY_F11:
      case XKB_KEY_F12:
      case XKB_KEY_F13:
      case XKB_KEY_F14:
      case XKB_KEY_F15:
      case XKB_KEY_F16:
      case XKB_KEY_F17:
      case XKB_KEY_F18:
      case XKB_KEY_F19:
      case XKB_KEY_F20:
      case XKB_KEY_F21:
      case XKB_KEY_F22:
      case XKB_KEY_F23:
      case XKB_KEY_F24:
        break;
      default:
        // Translate the keysym to a UTF-32 code point
        // and add it to the model if it's printable
        const char32_t utf32 = xkb_keysym_to_utf32(keysym);
        // Filter out non-printable characters
        if (utf32 >= 0x20 && utf32 != 0x7f) {
          active_model_->AddCodePoint(utf32);
          SendStateUpdate(*(active_model_));
        }
        break;
    }
  }
}

TextInputPlugin::TextInputPlugin(flutter::BinaryMessenger* messenger)
    : channel_(std::make_unique<flutter::MethodChannel<rapidjson::Document>>(
          messenger,
          kChannelName,
          &flutter::JsonMethodCodec::GetInstance())),
      active_model_(nullptr) {
  channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<rapidjson::Document>& call,
             const std::unique_ptr<flutter::MethodResult<rapidjson::Document>>&
                 result) { HandleMethodCall(call, result); });
}

TextInputPlugin::~TextInputPlugin() = default;

void TextInputPlugin::KeymapChanged(xkb_keymap* keymap) {
  const xkb_mod_index_t idx =
      xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
  if (idx != XKB_MOD_INVALID) {
    shift_mask_ = 1u << idx;
  }
}

void TextInputPlugin::HandleMethodCall(
    const flutter::MethodCall<rapidjson::Document>& method_call,
    const std::unique_ptr<flutter::MethodResult<rapidjson::Document>>& result) {
  const std::string& method = method_call.method_name();

  if (method == kShowMethod || method == kHideMethod) {
    // These methods are no-ops.
  } else if (method == kClearClientMethod) {
    active_model_ = nullptr;
  } else if (method == kSetClientMethod) {
    if (!method_call.arguments() || method_call.arguments()->IsNull()) {
      result->Error(kBadArgumentError, "Method invoked without args");
      return;
    }
    const rapidjson::Document& args = *method_call.arguments();

    // TODO(awdavies): There's quite a wealth of arguments supplied with this
    // method, and they should be inspected/used.
    const rapidjson::Value& client_id_json = args[0];
    const rapidjson::Value& client_config = args[1];
    if (client_id_json.IsNull()) {
      result->Error(kBadArgumentError, "Could not set client, ID is null.");
      return;
    }
    if (client_config.IsNull()) {
      result->Error(kBadArgumentError,
                    "Could not set client, missing arguments.");
      return;
    }
    client_id_ = client_id_json.GetInt();
    input_action_ = "";
    const auto input_action_json = client_config.FindMember(kTextInputAction);
    if (input_action_json != client_config.MemberEnd() &&
        input_action_json->value.IsString()) {
      input_action_ = input_action_json->value.GetString();
    }
    input_type_ = "";
    const auto input_type_info_json = client_config.FindMember(kTextInputType);
    if (input_type_info_json != client_config.MemberEnd() &&
        input_type_info_json->value.IsObject()) {
      const auto input_type_json =
          input_type_info_json->value.FindMember(kTextInputTypeName);
      if (input_type_json != input_type_info_json->value.MemberEnd() &&
          input_type_json->value.IsString()) {
        input_type_ = input_type_json->value.GetString();
      }
    }
    active_model_ = std::make_unique<TextInputModel>();
  } else if (method == kSetEditingStateMethod) {
    if (!method_call.arguments() || method_call.arguments()->IsNull()) {
      result->Error(kBadArgumentError, "Method invoked without args");
      return;
    }
    const rapidjson::Document& args = *method_call.arguments();

    if (active_model_ == nullptr) {
      result->Error(
          kInternalConsistencyError,
          "Set editing state has been invoked, but no client is set.");
      return;
    }
    const auto text = args.FindMember(kTextKey);
    if (text == args.MemberEnd() || text->value.IsNull()) {
      result->Error(kBadArgumentError,
                    "Set editing state has been invoked, but without text.");
      return;
    }
    const auto selection_base = args.FindMember(kSelectionBaseKey);
    const auto selection_extent = args.FindMember(kSelectionExtentKey);
    if (selection_base == args.MemberEnd() || selection_base->value.IsNull() ||
        selection_extent == args.MemberEnd() ||
        selection_extent->value.IsNull()) {
      result->Error(kInternalConsistencyError,
                    "Selection base/extent values invalid.");
      return;
    }
    // Flutter uses -1/-1 for invalid; translate that to 0/0 for the model.
    int base = selection_base->value.GetInt();
    int extent = selection_extent->value.GetInt();
    if (base == -1 && extent == -1) {
      base = extent = 0;
    }
    active_model_->SetText(text->value.GetString());
    active_model_->SetSelection(
        TextRange(static_cast<size_t>(base), static_cast<size_t>(extent)));
  } else {
    result->NotImplemented();
    return;
  }
  // All error conditions return early, so if nothing has gone wrong indicate
  // success.
  result->Success();
}

void TextInputPlugin::SendStateUpdate(const TextInputModel& model) const {
  auto args = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = args->GetAllocator();
  args->PushBack(client_id_, allocator);

  const TextRange selection = model.selection();
  rapidjson::Value editing_state(rapidjson::kObjectType);
  editing_state.AddMember(kComposingBaseKey, -1, allocator);
  editing_state.AddMember(kComposingExtentKey, -1, allocator);
  editing_state.AddMember(kSelectionAffinityKey, kAffinityDownstream,
                          allocator);
  editing_state.AddMember(kSelectionBaseKey, selection.base(), allocator);
  editing_state.AddMember(kSelectionExtentKey, selection.extent(), allocator);
  editing_state.AddMember(kSelectionIsDirectionalKey, false, allocator);
  editing_state.AddMember(
      kTextKey, rapidjson::Value(model.GetText(), allocator).Move(), allocator);
  args->PushBack(editing_state, allocator);

  channel_->InvokeMethod(kUpdateEditingStateMethod, std::move(args));
}

void TextInputPlugin::EnterPressed(TextInputModel* model) const {
  if (input_type_ == kMultilineInputType) {
    model->AddCodePoint('\n');
    SendStateUpdate(*model);
  }
  auto args = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = args->GetAllocator();
  args->PushBack(client_id_, allocator);
  args->PushBack(rapidjson::Value(input_action_, allocator).Move(), allocator);

  channel_->InvokeMethod(kPerformActionMethod, std::move(args));
}
}  // namespace flutter
