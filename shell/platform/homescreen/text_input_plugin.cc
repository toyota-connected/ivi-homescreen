// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shell/platform/homescreen/text_input_plugin.h"

#include <algorithm>

#include "flutter/shell/platform/common/json_method_codec.h"

#include "logging/logging.h"

static constexpr char kSetEditingStateMethod[] = "TextInput.setEditingState";
static constexpr char kClearClientMethod[] = "TextInput.clearClient";
static constexpr char kSetClientMethod[] = "TextInput.setClient";
static constexpr char kShowMethod[] = "TextInput.show";
static constexpr char kHideMethod[] = "TextInput.hide";
static constexpr char kSetEditableSizeAndTransformMethod[] =
    "TextInput.setEditableSizeAndTransform";
static constexpr char kSetMarkedTextRectMethod[] =
    "TextInput.setMarkedTextRect";

static constexpr char kMultilineInputType[] = "TextInputType.multiline";

// Keys used by the IME-positioning method calls.
static constexpr char kTransformKey[] = "transform";
static constexpr char kWidthKey[] = "width";
static constexpr char kHeightKey[] = "height";
static constexpr char kXKey[] = "x";
static constexpr char kYKey[] = "y";

// TextInputType.name values that map to a non-default IME content type.
static constexpr char kNumberInputType[] = "TextInputType.number";
static constexpr char kPhoneInputType[] = "TextInputType.phone";
static constexpr char kUrlInputType[] = "TextInputType.url";
static constexpr char kEmailInputType[] = "TextInputType.emailAddress";
static constexpr char kVisiblePasswordInputType[] =
    "TextInputType.visiblePassword";

static constexpr char kObscureTextKey[] = "obscureText";

// wl::ime::ContentHint underlying values (kept in sync with
// <wl/ime/text_input_receiver.hpp>; not included here to avoid a Wayland
// dependency in this otherwise backend-agnostic plugin).
static constexpr uint32_t kHintNone = 0x0;
static constexpr uint32_t kHintHiddenText = 0x40;
static constexpr uint32_t kHintSensitiveData = 0x80;
static constexpr uint32_t kHintMultiline = 0x200;

// wl::ime::ContentPurpose underlying values.
static constexpr uint32_t kPurposeNormal = 0;
static constexpr uint32_t kPurposeNumber = 3;
static constexpr uint32_t kPurposePhone = 4;
static constexpr uint32_t kPurposeUrl = 5;
static constexpr uint32_t kPurposeEmail = 6;
static constexpr uint32_t kPurposePassword = 8;

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

// Returns true if the UTF-32 codepoint is an ASCII hex digit (0-9, a-f, A-F).
static bool IsHexDigit(const char32_t c) {
  return (c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'f') ||
         (c >= U'A' && c <= U'F');
}

static constexpr char kBadArgumentError[] = "Bad Arguments";
static constexpr char kInternalConsistencyError[] =
    "Internal Consistency Error";

namespace flutter {

// Byte length (1–4) of the UTF-8 sequence whose leading byte is |c|.
static size_t Utf8SeqLen(const unsigned char c) {
  if (c < 0x80) {
    return 1;
  }
  if (c < 0xe0) {
    return 2;
  }
  if (c < 0xf0) {
    return 3;
  }
  return 4;  // supplementary plane → surrogate pair → 2 UTF-16 units
}

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
    const size_t seq_len = Utf8SeqLen(static_cast<unsigned char>(utf8[i]));
    u16 += (seq_len == 4) ? 2u : 1u;
    i += seq_len;
  }
  return u16;
}

// Inverse of Utf8PosToUtf16Index: UTF-16 code-unit index → UTF-8 byte offset.
// Clamped to the end of the string. Lets the arrow-key handlers derive the
// cursor byte offset from the already-fetched text instead of paying a second
// full UTF-16→UTF-8 conversion via TextInputModel::GetCursorOffset().
static size_t Utf16IndexToUtf8Pos(const std::string& utf8, size_t u16_index) {
  size_t u16 = 0;
  size_t i = 0;
  while (i < utf8.size() && u16 < u16_index) {
    const size_t seq_len = std::min(
        Utf8SeqLen(static_cast<unsigned char>(utf8[i])), utf8.size() - i);
    u16 += (seq_len == 4) ? 2u : 1u;
    i += seq_len;
  }
  return i;
}

// Number of Unicode codepoints in utf8[begin, end).
static size_t Utf8CodepointCount(const std::string& utf8,
                                 size_t begin,
                                 const size_t end) {
  size_t n = 0;
  for (size_t i = begin; i < end; ++n) {
    i += std::min(Utf8SeqLen(static_cast<unsigned char>(utf8[i])), end - i);
  }
  return n;
}

// Returns the byte offset that is |col| *codepoints* into the line starting at
// |line_start| in |text|, clamped to the line end (the '\n' or end of string).
// Counting in codepoints (not bytes) keeps the visual column stable across
// lines with multi-byte characters and guarantees the result lands on a
// codepoint boundary (so Utf8PosToUtf16Index never sees a partial sequence).
static size_t ClampedColumn(const std::string& text,
                            const size_t line_start,
                            const size_t col) {
  size_t i = line_start;
  for (size_t c = 0; c < col && i < text.size() && text[i] != '\n'; ++c) {
    // A '\n' is a single byte and never appears inside a multi-byte sequence,
    // so advancing a whole codepoint here can't step over the line end.
    i += std::min(Utf8SeqLen(static_cast<unsigned char>(text[i])),
                  text.size() - i);
  }
  return i;
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
  const size_t col = Utf8CodepointCount(text, cur_line_start, pos);

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
  const size_t col = Utf8CodepointCount(text, cur_line_start, pos);
  const size_t next_line_start = cur_nl + 1;

  return ClampedColumn(text, next_line_start, col);
}

std::string TextInputPlugin::GetSelectedText() const {
  if (active_model_ == nullptr) {
    return "";
  }
  return active_model_->GetSelectedText();
}

void TextInputPlugin::PasteText(const std::string& text) {
  if (active_model_ == nullptr || text.empty()) {
    return;
  }
  active_model_->SetSelectedText(text);
  SendStateUpdate(*active_model_);
}

void TextInputPlugin::DeleteSelectedText() {
  if (active_model_ == nullptr || active_model_->selection().collapsed()) {
    return;
  }
  active_model_->Delete();
  SendStateUpdate(*active_model_);
}

void TextInputPlugin::DeriveContentType(const bool obscure_text) {
  uint32_t hint = kHintNone;
  uint32_t purpose = kPurposeNormal;

  if (input_type_ == kMultilineInputType) {
    hint |= kHintMultiline;
  } else if (input_type_ == kNumberInputType) {
    purpose = kPurposeNumber;
  } else if (input_type_ == kPhoneInputType) {
    purpose = kPurposePhone;
  } else if (input_type_ == kUrlInputType) {
    purpose = kPurposeUrl;
  } else if (input_type_ == kEmailInputType) {
    purpose = kPurposeEmail;
  } else if (input_type_ == kVisiblePasswordInputType) {
    purpose = kPurposePassword;
  } else {
    // kTextInputTypeText and any unrecognized type map to the normal purpose.
    purpose = kPurposeNormal;
  }

  // obscureText (or an explicit password type) hides the text and marks it
  // sensitive so the IME suppresses learning/suggestions.
  if (obscure_text || purpose == kPurposePassword) {
    purpose = kPurposePassword;
    hint |= kHintHiddenText | kHintSensitiveData;
  }

  content_hint_ = hint;
  content_purpose_ = purpose;
}

void TextInputPlugin::CommitString(const std::string& utf8) {
  if (active_model_ == nullptr || utf8.empty()) {
    return;
  }
  // zwp_text_input_v3: a commit_string replaces any active preedit. The preedit
  // is discarded (not committed) and the commit text is inserted as committed
  // text. Erase the composing run first -- AddText() on an active composing run
  // would re-mark the inserted text as composing instead of committing it.
  if (active_model_->composing()) {
    active_model_->UpdateComposingText(std::string{});
    active_model_->EndComposing();
  }
  compose_base_utf16_ = compose_extent_utf16_ = -1;
  active_model_->AddText(utf8);
  UpdateClientAndIme();
}

void TextInputPlugin::SetPreeditString(const std::string& utf8,
                                       int /* cursor_begin */,
                                       int /* cursor_end */) {
  if (active_model_ == nullptr) {
    return;
  }
  if (utf8.empty()) {
    // Empty preedit clears any active composing run without committing it.
    if (active_model_->composing()) {
      active_model_->UpdateComposingText(std::string{});
      active_model_->CommitComposing();
      active_model_->EndComposing();
    }
    compose_base_utf16_ = compose_extent_utf16_ = -1;
  } else {
    if (!active_model_->composing()) {
      active_model_->BeginComposing();
    }
    active_model_->UpdateComposingText(utf8);
    const TextRange composing = active_model_->composing_range();
    compose_base_utf16_ = static_cast<int>(composing.base());
    compose_extent_utf16_ = static_cast<int>(composing.extent());
  }
  UpdateClientAndIme();
}

void TextInputPlugin::DeleteSurrounding(const int before, const int after) {
  if (active_model_ == nullptr) {
    return;
  }
  bool changed = false;
  // Delete the trailing region first so the leading deletion's negative offset
  // stays anchored to the original cursor position.
  if (after > 0) {
    changed |= active_model_->DeleteSurrounding(0, after);
  }
  if (before > 0) {
    changed |= active_model_->DeleteSurrounding(-before, before);
  }
  if (changed) {
    UpdateClientAndIme();
  }
}

void TextInputPlugin::CharHook(const unsigned int code_point) {
  if (active_model_ == nullptr) {
    return;
  }
  active_model_->AddCodePoint(code_point);
  UpdateClientAndIme();
}

// Applies one physical key event from the seat to the active text-input model.
// It first runs the compose/Unicode state machine (Ctrl+Shift+U hex entry and
// dead-key composition), then handles editing keys (arrows, Home/End,
// Backspace, Delete, Enter) with optional Shift-selection and Ctrl
// word-granularity, and finally inserts printable code points. Only key presses
// mutate state; key releases return early. Each mutation ends with
// SendStateUpdate so the Flutter framework sees the new selection and text.
void TextInputPlugin::KeyboardHook(bool released,
                                   xkb_keysym_t keysym,
                                   uint32_t /* xkb_scancode */,
                                   uint32_t modifiers) {
  if (active_model_ == nullptr) {
    return;
  }
  if (!released) {
    /* --- GTK-style Ctrl+Shift+U Unicode input ---
     * Activation: Ctrl+Shift+U (keysym XKB_KEY_U, capital U).
     *
     * While active:
     * - an underlined 'u' is shown in the text field
     * - hex digits are accumulated and shown after the 'u'
     * - other keys are ignored
     * - Enter commits the code point
     * - Backspace removes the last hex digit
     * - If no digits, Backspace cancels the mode
     * - Esc cancels the mode
     * - Focus loss cancels the mode
     *
     * Also see:
     * - https://www.freedesktop.org/wiki/Software/ibus/UnicodeInput/
     * - key_event_handler.cc: forwards FocusLost
     */
    // Ctrl+Shift+U activates Unicode input mode (IBus convention).
    // Accept both XKB_KEY_U (normal) and XKB_KEY_u (Caps Lock + Shift, where
    // Caps Lock and Shift cancel out and produce the lowercase keysym).
    if ((keysym == XKB_KEY_U || keysym == XKB_KEY_u) &&
        (modifiers & ctrl_mask_) != 0 && (modifiers & shift_mask_) != 0 &&
        unicode_state_ == UnicodeInputState::kNormal) {
      ActivateUnicodeInput();
      return;
    }
    // While in kPendingHex state intercept all keys for the state machine.
    if (unicode_state_ == UnicodeInputState::kPendingHex) {
      switch (keysym) {
        case XKB_KEY_Return:
        case XKB_KEY_ISO_Enter:
        case XKB_KEY_KP_Enter:
          CommitUnicodeInput();
          break;
        case XKB_KEY_Escape:
          CancelUnicodeInput();
          break;
        case XKB_KEY_BackSpace:
          if (!unicode_hex_.empty()) {
            // Remove the last hex digit from the model and the accumulator.
            active_model_->Backspace();
            unicode_hex_.pop_back();
            compose_extent_utf16_--;
            SendStateUpdate(*active_model_);
          } else {
            // Nothing accumulated yet; Backspace cancels the mode entirely.
            CancelUnicodeInput();
          }
          break;
        default: {
          const char32_t utf32 = xkb_keysym_to_utf32(keysym);
          if (unicode_hex_.size() < 6 && IsHexDigit(utf32)) {
            // Normalise to lowercase for display and stoul().
            const char32_t lower =
                (utf32 >= U'A' && utf32 <= U'F')
                    ? static_cast<char32_t>(utf32 - U'A' + U'a')
                    : utf32;
            unicode_hex_ += static_cast<char>(lower);
            active_model_->AddCodePoint(lower);
            compose_extent_utf16_++;
            SendStateUpdate(*active_model_);
          }
          // Non-hex keystrokes are silently ignored while in unicode mode.
          break;
        }
      }
      return;
    }
    // --- End Unicode input handling ---

    const bool shift = (modifiers & shift_mask_) != 0;
    switch (keysym) {
      case XKB_KEY_BackSpace:
        if (active_model_->Backspace()) {
          UpdateClientAndIme();
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
          UpdateClientAndIme();
        } else if (active_model_->MoveCursorBack()) {
          UpdateClientAndIme();
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
          UpdateClientAndIme();
        } else if (active_model_->MoveCursorForward()) {
          UpdateClientAndIme();
        }
        break;
      case XKB_KEY_Up:
      case XKB_KEY_KP_Up: {
        // Work in UTF-8 byte space (PreviousLinePosition), deriving the cursor
        // byte offset from the cursor's UTF-16 extent against the text we
        // already fetched, then convert the result back to a UTF-16 code-unit
        // index for SetSelection.
        const std::string text = active_model_->GetText();
        const size_t utf8_pos =
            Utf16IndexToUtf8Pos(text, active_model_->selection().extent());
        const size_t new_utf8_pos = PreviousLinePosition(text, utf8_pos);
        const size_t new_u16_pos = Utf8PosToUtf16Index(text, new_utf8_pos);
        if (new_u16_pos != active_model_->selection().extent()) {
          active_model_->SetSelection(
              shift ? TextRange(active_model_->selection().base(), new_u16_pos)
                    : TextRange(new_u16_pos));
          UpdateClientAndIme();
        }
        break;
      }
      case XKB_KEY_Down:
      case XKB_KEY_KP_Down: {
        const std::string text = active_model_->GetText();
        const size_t utf8_pos =
            Utf16IndexToUtf8Pos(text, active_model_->selection().extent());
        const size_t new_utf8_pos = NextLinePosition(text, utf8_pos);
        const size_t new_u16_pos = Utf8PosToUtf16Index(text, new_utf8_pos);
        if (new_u16_pos != active_model_->selection().extent()) {
          active_model_->SetSelection(
              shift ? TextRange(active_model_->selection().base(), new_u16_pos)
                    : TextRange(new_u16_pos));
          UpdateClientAndIme();
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
        UpdateClientAndIme();
        break;
      case XKB_KEY_Home:
      case XKB_KEY_KP_Home:
        if (shift) {
          active_model_->SelectToBeginning();
        } else {
          active_model_->MoveCursorToBeginning();
        }
        UpdateClientAndIme();
        break;
      case XKB_KEY_Delete:
      case XKB_KEY_KP_Delete:
        if (active_model_->Delete()) {
          UpdateClientAndIme();
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
        // and add it to the model if it's printable.
        // Suppress insertion when a ctrl modifier is active — that combination
        // is a keyboard shortcut (e.g. Ctrl+C), not text input.
        const char32_t utf32 = xkb_keysym_to_utf32(keysym);
        if (utf32 >= 0x20 && utf32 != 0x7f && (modifiers & ctrl_mask_) == 0) {
          active_model_->AddCodePoint(utf32);
          UpdateClientAndIme();
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
  const xkb_mod_index_t shift_idx =
      xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
  if (shift_idx != XKB_MOD_INVALID) {
    shift_mask_ = 1u << shift_idx;
  }
  const xkb_mod_index_t ctrl_idx =
      xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
  if (ctrl_idx != XKB_MOD_INVALID) {
    ctrl_mask_ = 1u << ctrl_idx;
  }
}

void TextInputPlugin::FocusLost() {
  if (active_model_ != nullptr) {
    CancelUnicodeInput();
  }
}

void TextInputPlugin::ActivateUnicodeInput() {
  if (active_model_ == nullptr) {
    return;
  }
  unicode_state_ = UnicodeInputState::kPendingHex;
  unicode_hex_.clear();
  // Record the cursor position (UTF-16 units) before inserting 'u'.
  const int cursor = static_cast<int>(active_model_->selection().position());
  active_model_->AddCodePoint(U'u');
  compose_base_utf16_ = cursor;
  compose_extent_utf16_ = cursor + 1;
  SendStateUpdate(*active_model_);
}

void TextInputPlugin::CommitUnicodeInput() {
  if (active_model_ == nullptr) {
    unicode_state_ = UnicodeInputState::kNormal;
    unicode_hex_.clear();
    compose_base_utf16_ = compose_extent_utf16_ = -1;
    return;
  }
  // Remove the composing 'u' + accumulated hex digit characters.
  const size_t remove_count = 1 + unicode_hex_.size();
  for (size_t i = 0; i < remove_count; ++i) {
    active_model_->Backspace();
  }
  compose_base_utf16_ = compose_extent_utf16_ = -1;
  unicode_state_ = UnicodeInputState::kNormal;

  if (!unicode_hex_.empty()) {
    unsigned long codepoint = 0;
    try {
      codepoint = std::stoul(unicode_hex_, nullptr, 16);
    } catch (...) {
      // Make malformed hex observable so it can be debugged.
      spdlog::warn("[text] CommitUnicodeInput: malformed hex '{}'; dropping",
                   unicode_hex_);
    }
    // Reject U+0000, surrogates (U+D800–U+DFFF), and values above U+10FFFF.
    const bool valid = codepoint > 0 && codepoint <= 0x10FFFF &&
                       !(codepoint >= 0xD800 && codepoint <= 0xDFFF);
    if (valid) {
      active_model_->AddCodePoint(static_cast<char32_t>(codepoint));
    }
  }
  unicode_hex_.clear();
  SendStateUpdate(*active_model_);
}

void TextInputPlugin::CancelUnicodeInput() {
  if (unicode_state_ == UnicodeInputState::kNormal) {
    return;
  }
  if (active_model_ != nullptr) {
    // Remove the composing 'u' + any accumulated hex digit characters.
    const size_t remove_count = 1 + unicode_hex_.size();
    for (size_t i = 0; i < remove_count; ++i) {
      active_model_->Backspace();
    }
    compose_base_utf16_ = compose_extent_utf16_ = -1;
    SendStateUpdate(*active_model_);
  }
  unicode_state_ = UnicodeInputState::kNormal;
  unicode_hex_.clear();
  compose_base_utf16_ = compose_extent_utf16_ = -1;
}

// Dispatches calls on the flutter/textinput method channel. TextInput.show and
// TextInput.hide toggle the (no-op) on-screen keyboard; TextInput.setClient
// records the connection id and input configuration; TextInput.setEditingState
// rebuilds active_model_ from the framework's text/selection/composing range;
// and TextInput.clearClient drops the active connection. Unknown methods are
// answered with NotImplemented and every handled call sends an empty success
// reply unless noted otherwise.
void TextInputPlugin::HandleMethodCall(
    const flutter::MethodCall<rapidjson::Document>& method_call,
    const std::unique_ptr<flutter::MethodResult<rapidjson::Document>>& result) {
  const std::string& method = method_call.method_name();

  if (method == kShowMethod) {
    // A text field requested the keyboard: enable the compositor IME with the
    // content type derived at setClient time and the last-known cursor rect.
    if (ime_activate_) {
      ime_activate_(content_hint_, content_purpose_, cursor_rect_x_,
                    cursor_rect_y_, cursor_rect_w_, cursor_rect_h_);
    }
    // Seed the IME with the field's current committed text + cursor so it can
    // delete surrounding text from the first keystroke. Invalidate the dedupe
    // cache first so the seed always goes through on (re)focus.
    last_surrounding_valid_ = false;
    last_cursor_rect_valid_ = false;
    SendSurroundingToIme();
  } else if (method == kHideMethod) {
    if (ime_deactivate_) {
      ime_deactivate_();
    }
  } else if (method == kSetEditableSizeAndTransformMethod) {
    // Flutter sends the editable box size + a column-major 4x4 transform; the
    // translation (elements 12/13) is the on-screen origin. Use that box as the
    // IME anchor rectangle.
    if (method_call.arguments() && !method_call.arguments()->IsNull()) {
      const rapidjson::Document& args = *method_call.arguments();
      const auto width = args.FindMember(kWidthKey);
      const auto height = args.FindMember(kHeightKey);
      const auto transform = args.FindMember(kTransformKey);
      if (width != args.MemberEnd() && width->value.IsNumber() &&
          height != args.MemberEnd() && height->value.IsNumber()) {
        cursor_rect_w_ = static_cast<int32_t>(width->value.GetDouble());
        cursor_rect_h_ = static_cast<int32_t>(height->value.GetDouble());
      }
      if (transform != args.MemberEnd() && transform->value.IsArray() &&
          transform->value.Size() >= 14) {
        cursor_rect_x_ = static_cast<int32_t>(transform->value[12].GetDouble());
        cursor_rect_y_ = static_cast<int32_t>(transform->value[13].GetDouble());
      }
      EmitCursorRect();
    }
  } else if (method == kSetMarkedTextRectMethod) {
    // A marked-text (composing) rectangle in logical pixels; anchor the IME
    // candidate window here.
    if (method_call.arguments() && !method_call.arguments()->IsNull()) {
      const rapidjson::Document& args = *method_call.arguments();
      const auto x = args.FindMember(kXKey);
      const auto y = args.FindMember(kYKey);
      const auto width = args.FindMember(kWidthKey);
      const auto height = args.FindMember(kHeightKey);
      if (x != args.MemberEnd() && x->value.IsNumber() &&
          y != args.MemberEnd() && y->value.IsNumber() &&
          width != args.MemberEnd() && width->value.IsNumber() &&
          height != args.MemberEnd() && height->value.IsNumber()) {
        cursor_rect_x_ = static_cast<int32_t>(x->value.GetDouble());
        cursor_rect_y_ = static_cast<int32_t>(y->value.GetDouble());
        cursor_rect_w_ = static_cast<int32_t>(width->value.GetDouble());
        cursor_rect_h_ = static_cast<int32_t>(height->value.GetDouble());
        EmitCursorRect();
      }
    }
  } else if (method == kClearClientMethod) {
    CancelUnicodeInput();
    if (ime_deactivate_) {
      ime_deactivate_();
    }
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
    // Reject non-integer types before calling GetInt()
    if (!client_id_json.IsInt()) {
      result->Error(kBadArgumentError,
                    "Could not set client, ID is not an integer.");
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
    // Derive the compositor IME content hint/purpose from the input type and
    // the obscureText flag so a subsequent TextInput.show activates correctly.
    bool obscure_text = false;
    const auto obscure_json = client_config.FindMember(kObscureTextKey);
    if (obscure_json != client_config.MemberEnd() &&
        obscure_json->value.IsBool()) {
      obscure_text = obscure_json->value.GetBool();
    }
    DeriveContentType(obscure_text);
    // Reset any in-progress Unicode input state from the previous client.
    unicode_state_ = UnicodeInputState::kNormal;
    unicode_hex_.clear();
    compose_base_utf16_ = compose_extent_utf16_ = -1;
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
    if (text == args.MemberEnd() || text->value.IsNull() ||
        !text->value.IsString()) {
      result->Error(kBadArgumentError,
                    "Set editing state has been invoked, but without text.");
      return;
    }
    const auto selection_base = args.FindMember(kSelectionBaseKey);
    const auto selection_extent = args.FindMember(kSelectionExtentKey);
    if (selection_base == args.MemberEnd() || selection_base->value.IsNull() ||
        selection_extent == args.MemberEnd() ||
        selection_extent->value.IsNull() || !selection_base->value.IsInt() ||
        !selection_extent->value.IsInt()) {
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

    // Reject negative values other than the -1/-1 sentinel (already
    // resolved above) and clamp to [0, utf16_text_length] to prevent
    // out-of-range indices from reaching arrow-key navigation math.
    if (base < 0 || extent < 0) {
      result->Error(kBadArgumentError, "Selection base/extent must be >= 0.");
      return;
    }

    const std::string text_str(text->value.GetString(),
                               text->value.GetStringLength());
    const size_t utf16_len = Utf8PosToUtf16Index(text_str, text_str.size());
    const size_t clamped_base = std::min(static_cast<size_t>(base), utf16_len);
    const size_t clamped_extent =
        std::min(static_cast<size_t>(extent), utf16_len);
    active_model_->SetText(text_str);
    active_model_->SetSelection(TextRange(clamped_base, clamped_extent));
    // The framework rebuilt the field's committed text + selection; keep the
    // compositor IME's surrounding-text view in sync.
    SendSurroundingToIme();
    // Reset the state machine here if the framework calls
    // setEditingState while Unicode input is in progress.  This can happen if
    // the framework tries to Prevents Commit/CancelUnicodeInput corruption of
    // user-typed text.
    unicode_state_ = UnicodeInputState::kNormal;
    unicode_hex_.clear();
    compose_base_utf16_ = compose_extent_utf16_ = -1;
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
  editing_state.AddMember(kComposingBaseKey, compose_base_utf16_, allocator);
  editing_state.AddMember(kComposingExtentKey, compose_extent_utf16_,
                          allocator);
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

void TextInputPlugin::SendSurroundingToIme() const {
  if (active_model_ == nullptr || !ime_update_surrounding_) {
    return;
  }

  // GetText() returns the full UTF-8 text *including* the active preedit
  // (composing) substring. zwp_text_input_v3's set_surrounding_text must carry
  // only the committed text, so excise the composing run before sending.
  const std::string full = active_model_->GetText();

  // selection() base/extent and composing_range() are UTF-16 code-unit indices.
  // Convert each to a UTF-8 byte offset within |full|: Utf16IndexToUtf8Pos
  // converts the UTF-16 prefix [0, index) to UTF-8 and yields its byte length,
  // which is exactly the byte offset of that index.
  size_t cursor_bytes =
      Utf16IndexToUtf8Pos(full, active_model_->selection().extent());
  size_t anchor_bytes =
      Utf16IndexToUtf8Pos(full, active_model_->selection().base());

  std::string committed = full;
  if (active_model_->composing()) {
    const TextRange composing = active_model_->composing_range();
    if (!composing.collapsed()) {
      const size_t comp_begin = Utf16IndexToUtf8Pos(full, composing.start());
      const size_t comp_end = Utf16IndexToUtf8Pos(full, composing.end());
      if (comp_end > comp_begin && comp_end <= full.size()) {
        const size_t comp_len = comp_end - comp_begin;
        committed.erase(comp_begin, comp_len);
        // Shift offsets that fall after the removed region back by its length;
        // an offset inside the region collapses to its start.
        const auto adjust = [&](size_t off) -> size_t {
          if (off >= comp_end) {
            return off - comp_len;
          }
          if (off > comp_begin) {
            return comp_begin;
          }
          return off;
        };
        cursor_bytes = adjust(cursor_bytes);
        anchor_bytes = adjust(anchor_bytes);
      }
    }
  }

  cursor_bytes = std::min(cursor_bytes, committed.size());
  anchor_bytes = std::min(anchor_bytes, committed.size());

  // zwp_text_input_v3 limits set_surrounding_text to 4000 bytes. The common
  // case (a short field) sends the whole committed text; only when it grows
  // large do we send a window centered on the cursor, which is all the IME
  // needs to delete characters adjacent to the caret. The window is snapped to
  // UTF-8 codepoint boundaries so a multi-byte sequence is never split.
  static constexpr size_t kMaxSurroundingBytes = 3000;
  if (committed.size() > kMaxSurroundingBytes) {
    const size_t half = kMaxSurroundingBytes / 2;
    size_t begin = cursor_bytes > half ? cursor_bytes - half : 0;
    size_t end = std::min(begin + kMaxSurroundingBytes, committed.size());
    const auto is_continuation = [&](size_t i) {
      return (static_cast<unsigned char>(committed[i]) & 0xC0) == 0x80;
    };
    while (begin < end && is_continuation(begin)) {
      ++begin;
    }
    while (end < committed.size() && is_continuation(end)) {
      ++end;
    }
    std::string window = committed.substr(begin, end - begin);
    const size_t cursor_w = cursor_bytes >= begin
                                ? std::min(cursor_bytes - begin, window.size())
                                : 0;
    const size_t anchor_w = anchor_bytes >= begin
                                ? std::min(anchor_bytes - begin, window.size())
                                : 0;
    EmitSurrounding(window, static_cast<uint32_t>(cursor_w),
                    static_cast<uint32_t>(anchor_w));
    return;
  }

  EmitSurrounding(committed, static_cast<uint32_t>(cursor_bytes),
                  static_cast<uint32_t>(anchor_bytes));
}

void TextInputPlugin::EmitSurrounding(const std::string& text,
                                      uint32_t cursor,
                                      uint32_t anchor) const {
  if (last_surrounding_valid_ && text == last_surrounding_text_ &&
      cursor == last_surrounding_cursor_ &&
      anchor == last_surrounding_anchor_) {
    return;
  }
  last_surrounding_text_ = text;
  last_surrounding_cursor_ = cursor;
  last_surrounding_anchor_ = anchor;
  last_surrounding_valid_ = true;
  ime_update_surrounding_(text, cursor, anchor);
}

void TextInputPlugin::EmitCursorRect() const {
  if (!ime_update_cursor_rect_) {
    return;
  }
  // Flutter re-sends setEditableSizeAndTransform every frame; the IME
  // re-evaluates on each set_cursor_rectangle+commit and answers with a
  // preedit, so an unchanged rectangle loops. Send only when it changes.
  if (last_cursor_rect_valid_ && cursor_rect_x_ == last_cursor_rect_x_ &&
      cursor_rect_y_ == last_cursor_rect_y_ &&
      cursor_rect_w_ == last_cursor_rect_w_ &&
      cursor_rect_h_ == last_cursor_rect_h_) {
    return;
  }
  last_cursor_rect_x_ = cursor_rect_x_;
  last_cursor_rect_y_ = cursor_rect_y_;
  last_cursor_rect_w_ = cursor_rect_w_;
  last_cursor_rect_h_ = cursor_rect_h_;
  last_cursor_rect_valid_ = true;
  ime_update_cursor_rect_(cursor_rect_x_, cursor_rect_y_, cursor_rect_w_,
                          cursor_rect_h_);
}

void TextInputPlugin::UpdateClientAndIme() const {
  SendStateUpdate(*active_model_);
  SendSurroundingToIme();
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
