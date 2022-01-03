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

#include <iostream>

//#include <flutter/shell/platform/common/client_wrapper/include/flutter/standard_method_codec.h>

#include "engine.h"

#define GLFW_RELEASE 0
#define GLFW_PRESS 1
#define GLFW_REPEAT 2

#define GLFW_KEY_ENTER 257
#define GLFW_KEY_BACKSPACE 259
#define GLFW_KEY_DELETE 261
#define GLFW_KEY_RIGHT 262
#define GLFW_KEY_LEFT 263
#define GLFW_KEY_DOWN 264
#define GLFW_KEY_UP 265
#define GLFW_KEY_HOME 268
#define GLFW_KEY_END 269

void TextInput::CharHook(unsigned int code_point) {
  if (active_model_ == nullptr) {
    return;
  }
  active_model_->AddCodePoint(code_point);
  SendStateUpdate(*active_model_);
}

void TextInput::KeyboardHook(int key,
                             int scancode,
                             int action,
                             int mods) {
  if (active_model_ == nullptr) {
    return;
  }
  if (action == GLFW_PRESS || action == GLFW_REPEAT) {
    switch (key) {
      case GLFW_KEY_LEFT:
        if (active_model_->MoveCursorBack()) {
          SendStateUpdate(*active_model_);
        }
        break;
      case GLFW_KEY_RIGHT:
        if (active_model_->MoveCursorForward()) {
          SendStateUpdate(*active_model_);
        }
        break;
      case GLFW_KEY_END:
        active_model_->MoveCursorToEnd();
        SendStateUpdate(*active_model_);
        break;
      case GLFW_KEY_HOME:
        active_model_->MoveCursorToBeginning();
        SendStateUpdate(*active_model_);
        break;
      case GLFW_KEY_BACKSPACE:
        if (active_model_->Backspace()) {
          SendStateUpdate(*active_model_);
        }
        break;
      case GLFW_KEY_DELETE:
        if (active_model_->Delete()) {
          SendStateUpdate(*active_model_);
        }
        break;
      case GLFW_KEY_ENTER:
        EnterPressed(active_model_.get());
        break;
      default:
        break;
    }
  }
}

void TextInput::SendStateUpdate(const flutter::TextInputModel& model) {
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
      kTextKey, rapidjson::Value(model.GetText().c_str(), allocator).Move(), allocator);
  args->PushBack(editing_state, allocator);

//  channel_->InvokeMethod(kUpdateEditingStateMethod, std::move(args));
}

void TextInput::EnterPressed(flutter::TextInputModel* model) {
  if (input_type_ == kMultilineInputType) {
    model->AddCodePoint('\n');
    SendStateUpdate(*model);
  }
  auto args = std::make_unique<rapidjson::Document>(rapidjson::kArrayType);
  auto& allocator = args->GetAllocator();
  args->PushBack(client_id_, allocator);
  args->PushBack(rapidjson::Value(input_action_.c_str(), allocator).Move(), allocator);

//  channel_->InvokeMethod(kPerformActionMethod, std::move(args));
}

void TextInput::OnPlatformMessage(const FlutterPlatformMessage* message,
                                  void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::JsonMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);
  auto method = obj->method_name();

  if (method == kShowMethod || method == kHideMethod) {
    // These methods are no-ops.
  } else if (method == kClearClientMethod) {
    active_model_ = nullptr;
  } else if (method == kSetClientMethod) {
    if (obj->arguments()->IsNull()) {
      auto res = codec.EncodeErrorEnvelope(kBadArgumentError, "Method invoked without args");
      engine->SendPlatformMessageResponse(message->response_handle, res->data(), res->size());
      return;
    }
    const rapidjson::Document& args = *(obj->arguments());

    // TODO(awdavies): There's quite a wealth of arguments supplied with this
    // method, and they should be inspected/used.
    const rapidjson::Value& client_id_json = args[0];
    const rapidjson::Value& client_config = args[1];
    if (client_id_json.IsNull()) {
      auto res = codec.EncodeErrorEnvelope(kBadArgumentError, "Could not set client, ID is null.");
      engine->SendPlatformMessageResponse(message->response_handle, res->data(), res->size());
      return;
    }
    if (client_config.IsNull()) {
      auto res = codec.EncodeErrorEnvelope(kBadArgumentError,
                    "Could not set client, missing arguments.");
      engine->SendPlatformMessageResponse(message->response_handle, res->data(), res->size());
      return;
    }
    client_id_ = client_id_json.GetInt();
    input_action_ = "";
    auto input_action_json = client_config.FindMember(kTextInputAction);
    if (input_action_json != client_config.MemberEnd() &&
        input_action_json->value.IsString()) {
      input_action_ = input_action_json->value.GetString();
    }
    input_type_ = "";
    auto input_type_info_json = client_config.FindMember(kTextInputType);
    if (input_type_info_json != client_config.MemberEnd() &&
        input_type_info_json->value.IsObject()) {
      auto input_type_json =
          input_type_info_json->value.FindMember(kTextInputTypeName);
      if (input_type_json != input_type_info_json->value.MemberEnd() &&
          input_type_json->value.IsString()) {
        input_type_ = input_type_json->value.GetString();
      }
    }
    active_model_ = std::make_unique<flutter::TextInputModel>();
  } else if (method == kSetEditingStateMethod) {
    if (!obj->arguments() || obj->arguments()->IsNull()) {
      auto res = codec.EncodeErrorEnvelope(kBadArgumentError, "Method invoked without args");
      engine->SendPlatformMessageResponse(message->response_handle, res->data(), res->size());
      return;
    }
    const rapidjson::Document& args = *(obj->arguments());

    if (active_model_ == nullptr) {
      auto res = codec.EncodeErrorEnvelope(kInternalConsistencyError,
                                            "Set editing state has been invoked, but no client is set.");
      engine->SendPlatformMessageResponse(message->response_handle, res->data(), res->size());
      return;
    }
    auto text = args.FindMember(kTextKey);
    if (text == args.MemberEnd() || text->value.IsNull()) {
      auto res = codec.EncodeErrorEnvelope(kBadArgumentError,
                                            "Set editing state has been invoked, but without text.");
      engine->SendPlatformMessageResponse(message->response_handle, res->data(), res->size());
      return;
    }
    auto selection_base = args.FindMember(kSelectionBaseKey);
    auto selection_extent = args.FindMember(kSelectionExtentKey);
    if (selection_base == args.MemberEnd() || selection_base->value.IsNull() ||
        selection_extent == args.MemberEnd() ||
        selection_extent->value.IsNull()) {
      auto res = codec.EncodeErrorEnvelope(kInternalConsistencyError,
                                            "Selection base/extent values invalid.");
      engine->SendPlatformMessageResponse(message->response_handle, res->data(), res->size());
      return;
    }
    // Flutter uses -1/-1 for invalid; translate that to 0/0 for the model.
    int base = selection_base->value.GetInt();
    int extent = selection_extent->value.GetInt();
    if (base == -1 && extent == -1) {
      base = extent = 0;
    }
    active_model_->SetText(text->value.GetString());
    active_model_->SetSelection(flutter::TextRange(base, extent));
  } else {
    engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
  }
  // All error conditions return early, so if nothing has gone wrong indicate
  // success.
  auto res = codec.EncodeSuccessEnvelope();
  engine->SendPlatformMessageResponse(message->response_handle, res->data(), res->size());
}

TextInput::TextInput(flutter::BinaryMessenger* messenger) {}
