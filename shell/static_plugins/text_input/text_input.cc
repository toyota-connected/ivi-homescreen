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
#include <cstring>

#include <flutter/fml/logging.h>
#include <flutter/json_method_codec.h>

#include "engine.h"

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

static constexpr char kBadArgumentError[] = "Bad Arguments";
static constexpr char kInternalConsistencyError[] =
    "Internal Consistency Error";

constexpr char kMethod[] = "method";
constexpr char kArgs[] = "args";

void TextInput::OnPlatformMessage(const FlutterPlatformMessage* message,
                                  void* userdata) {
  (void)userdata;
  auto engine = reinterpret_cast<Engine*>(userdata);
  rapidjson::Document document;
  document.Parse(reinterpret_cast<const char*>(message->message),
                 message->message_size);
  if (document.HasParseError() || !document.IsObject()) {
    return;
  }

  constexpr char kMethodSetClient[] = "TextInput.setClient";
  constexpr char kMethodShow[] = "TextInput.show";
  constexpr char kMethodSetEditableSizeAndTransform[] =
      "TextInput.setEditableSizeAndTransform";
  constexpr char kMethodSetStyle[] = "TextInput.setStyle";
  constexpr char kMethodSetEditingState[] = "TextInput.setEditingState";

  if (document.HasMember(kMethod) && document[kMethod].IsString()) {
    const char* method = document[kMethod].GetString();

    if (document.HasMember(kArgs) && document[kArgs].IsObject()) {
      auto args = document[kArgs].GetObject();

      if (0 == strcmp(kMethodSetClient, method)) {
        FML_DLOG(INFO) << "TextInput.setClient";

      } else if (0 == strcmp(kMethodShow, method)) {
        FML_DLOG(INFO) << "TextInput.show";

      } else if (0 == strcmp(kMethodSetEditableSizeAndTransform, method)) {
        FML_DLOG(INFO) << "TextInput.setEditableSizeAndTransform";

      } else if (0 == strcmp(kMethodSetStyle, method)) {
        FML_DLOG(INFO) << "TextInput.setEditableSizeAndTransform";

      } else if (0 == strcmp(kMethodSetEditingState, method)) {
        FML_DLOG(INFO) << "TextInput.setEditingState";

      } else if (0 == strcmp(kMethodShow, method)) {
      } else {
        FML_LOG(ERROR) << "TextInput Unhandled Method: " << method;
      }
    }
  }
  engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
}