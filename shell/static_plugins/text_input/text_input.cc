// Copyright 2020 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "text_input.h"

#include <memory>
#include "app.h"
#include "engine.h"

TextInput::TextInput()
    : client_id_(0),
      channel_(std::make_unique<flutter::MethodChannel<rapidjson::Document>>(
          this,
          kChannelName,
          &flutter::JsonMethodCodec::GetInstance())) {}

void TextInput::SendStateUpdate(const flutter::TextInputModel& model) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto args = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = args->GetAllocator();
  args->PushBack(client_id_, allocator);

  flutter::TextRange selection = model.selection();
  rapidjson::Value editing_state(rapidjson::kObjectType);
  editing_state.AddMember(kComposingBaseKey, -1, allocator);
  editing_state.AddMember(kComposingExtentKey, -1, allocator);
  editing_state.AddMember(kSelectionAffinityKey, kAffinityDownstream,
                          allocator);
  editing_state.AddMember(kSelectionBaseKey, selection.base(), allocator);
  editing_state.AddMember(kSelectionExtentKey, selection.extent(), allocator);
  editing_state.AddMember(kSelectionIsDirectionalKey, false, allocator);
  editing_state.AddMember(
      kTextKey, rapidjson::Value(model.GetText().c_str(), allocator).Move(),
      allocator);
  args->PushBack(editing_state, allocator);

  channel_->InvokeMethod(kUpdateEditingStateMethod, std::move(args));
}

void TextInput::EnterPressed(flutter::TextInputModel* model) {
  if (input_type_ == kMultilineInputType) {
    model->AddCodePoint('\n');
    SendStateUpdate(*model);
  }
  auto args = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = args->GetAllocator();
  args->PushBack(client_id_, allocator);
  args->PushBack(rapidjson::Value(input_action_.c_str(), allocator).Move(),
                 allocator);

  channel_->InvokeMethod(kPerformActionMethod, std::move(args));
}

void TextInput::SetEngine(const std::shared_ptr<Engine>& engine) {
  if (engine) {
    engine_ = engine;
    engine->SetTextInput(this);
  }
}

void TextInput::OnPlatformMessage(const FlutterPlatformMessage* message,
                                  void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto text_input = engine->GetTextInput();
  auto& codec = flutter::JsonMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);
  auto method = obj->method_name();

  if (method == kShowMethod || method == kHideMethod) {
    // These methods are no-ops.
  } else if (method == kClearClientMethod) {
    text_input->active_model_ = nullptr;
  } else if (method == kSetClientMethod) {
    if (obj->arguments()->IsNull()) {
      auto res = codec.EncodeErrorEnvelope(kBadArgumentError,
                                           "Method invoked without args");
      goto done;
    }
    const rapidjson::Document& args = *(obj->arguments());

    const rapidjson::Value& client_id_json = args[0];
    const rapidjson::Value& client_config = args[1];
    if (client_id_json.IsNull()) {
      auto res = codec.EncodeErrorEnvelope(kBadArgumentError,
                                           "Could not set client, ID is null.");
      goto done;
    }
    if (client_config.IsNull()) {
      auto res = codec.EncodeErrorEnvelope(
          kBadArgumentError, "Could not set client, missing arguments.");
      goto done;
    }
    text_input->client_id_ = client_id_json.GetInt();
    text_input->input_action_ = "";
    auto input_action_json = client_config.FindMember(kTextInputAction);
    if (input_action_json != client_config.MemberEnd() &&
        input_action_json->value.IsString()) {
      text_input->input_action_ = input_action_json->value.GetString();
    }
    text_input->input_type_ = "";
    auto input_type_info_json = client_config.FindMember(kTextInputType);
    if (input_type_info_json != client_config.MemberEnd() &&
        input_type_info_json->value.IsObject()) {
      auto input_type_json =
          input_type_info_json->value.FindMember(kTextInputTypeName);
      if (input_type_json != input_type_info_json->value.MemberEnd() &&
          input_type_json->value.IsString()) {
        text_input->input_type_ = input_type_json->value.GetString();
      }
    }
    text_input->active_model_ = std::make_unique<flutter::TextInputModel>();
  } else if (method == kSetEditingStateMethod) {
    if (!obj->arguments() || obj->arguments()->IsNull()) {
      result = codec.EncodeErrorEnvelope(kBadArgumentError,
                                         "Method invoked without args");
      goto done;
    }
    const rapidjson::Document& args = *(obj->arguments());

    if (text_input->active_model_ == nullptr) {
      result = codec.EncodeErrorEnvelope(
          kInternalConsistencyError,
          "Set editing state has been invoked, but no client is set.");
      goto done;
    }
    auto text = args.FindMember(kTextKey);
    if (text == args.MemberEnd() || text->value.IsNull()) {
      result = codec.EncodeErrorEnvelope(
          kBadArgumentError,
          "Set editing state has been invoked, but without text.");
      goto done;
    }
    auto selection_base = args.FindMember(kSelectionBaseKey);
    auto selection_extent = args.FindMember(kSelectionExtentKey);
    if (selection_base == args.MemberEnd() || selection_base->value.IsNull() ||
        selection_extent == args.MemberEnd() ||
        selection_extent->value.IsNull()) {
      result = codec.EncodeErrorEnvelope(
          kInternalConsistencyError, "Selection base/extent values invalid.");
      goto done;
    }
    // Flutter uses -1/-1 for invalid; translate that to 0/0 for the model.
    int base = selection_base->value.GetInt();
    int extent = selection_extent->value.GetInt();
    if (base == -1 && extent == -1) {
      base = extent = 0;
    }
    text_input->active_model_->SetText(text->value.GetString());
    text_input->active_model_->SetSelection(flutter::TextRange(base, extent));
  } else {
    engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
    return;
  }
  // All error conditions return early, so if nothing has gone wrong indicate
  // success.
  result = codec.EncodeSuccessEnvelope();

done:
  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}

void TextInput::keyboard_handle_key(void* data,
                                    struct wl_keyboard* keyboard,
                                    uint32_t serial,
                                    uint32_t time,
                                    xkb_keysym_t keysym,
                                    uint32_t state) {
  auto* text_input = static_cast<TextInput*>(data);

  if (text_input->active_model_ == nullptr) {
    return;
  }

  if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    switch (keysym) {
      case XKB_KEY_Left:
        if (text_input->active_model_->MoveCursorBack()) {
          text_input->SendStateUpdate(*text_input->active_model_);
        }
        break;
      case XKB_KEY_Right:
        if (text_input->active_model_->MoveCursorForward()) {
          text_input->SendStateUpdate(*text_input->active_model_);
        }
        break;
      case XKB_KEY_End:
        text_input->active_model_->MoveCursorToEnd();
        text_input->SendStateUpdate(*text_input->active_model_);
        break;
      case XKB_KEY_Home:
        text_input->active_model_->MoveCursorToBeginning();
        text_input->SendStateUpdate(*text_input->active_model_);
        break;
      case XKB_KEY_BackSpace:
        if (text_input->active_model_->Backspace()) {
          text_input->SendStateUpdate(*text_input->active_model_);
        }
        break;
      case XKB_KEY_Delete:
        if (text_input->active_model_->Delete()) {
          text_input->SendStateUpdate(*text_input->active_model_);
        }
        break;
      case XKB_KEY_ISO_Enter:
      case XKB_KEY_KP_Enter:
        text_input->EnterPressed(text_input->active_model_.get());
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
      case XKB_KEY_Return:
      case XKB_KEY_Pause:
      case XKB_KEY_Scroll_Lock:
      case XKB_KEY_Sys_Req:
      case XKB_KEY_Escape:
        break;
      default:
        text_input->active_model_->AddCodePoint(keysym);
        text_input->SendStateUpdate(*(text_input->active_model_));
        break;
    }

#if !defined(NDEBUG)
    uint32_t utf32 = xkb_keysym_to_utf32(keysym);
    if (utf32) {
      FML_DLOG(INFO) << "[Press] U" << utf32;
    } else {
      char name[64];
      xkb_keysym_get_name(keysym, name, 64);
      FML_DLOG(INFO) << "[Press] " << name;
    }
#endif
  }
}

void TextInput::Send(const std::string& channel,
                     const uint8_t* message,
                     size_t message_size,
                     flutter::BinaryReply reply) const {
  engine_->SendPlatformMessage(channel.c_str(), message, message_size);
  last_reply_handler_ = reply;
}

void TextInput::SetMessageHandler(const std::string& channel,
                                  flutter::BinaryMessageHandler handler) {
  last_message_handler_channel_ = channel;
  last_message_handler_ = handler;
}
