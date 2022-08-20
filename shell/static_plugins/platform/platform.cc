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

#include "platform.h"

#include <flutter/fml/logging.h>
#include <flutter/shell/platform/common/json_method_codec.h>

#include "engine.h"

std::string g_clipboard;

void Platform::OnPlatformMessage(const FlutterPlatformMessage* message,
                                 void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto& codec = flutter::JsonMethodCodec::GetInstance();
  auto obj = codec.DecodeMethodCall(message->message, message->message_size);

  auto method = obj->method_name();
  auto args = obj->arguments();

  if (method == kMethodSetApplicationSwitcherDescription) {
    if (!args->IsNull() && args->HasMember("label") &&
        args->HasMember("primaryColor")) {
      MethodSetApplicationSwitcherDescription description{};
      description.label = (*args)["label"].GetString();
      description.primaryColor = (*args)["primaryColor"].GetUint();
      FML_DLOG(INFO)
          << "(" << engine->GetIndex()
          << ") Platform: ApplicationSwitcherDescription\n\tlabel: \""
          << description.label
          << "\"\n\tprimaryColor: " << description.primaryColor;
      result = codec.EncodeSuccessEnvelope();
    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  } else if (method == kMethodClipboardHasStrings) {
    if (!args->IsNull() && args->IsString()) {
      auto format = args->GetString();
      if (0 == strcmp(format, kTextPlainFormat)) {
        rapidjson::Document res;
        res.SetObject();
        auto& allocator = res.GetAllocator();

        FML_DLOG(INFO)
            << "(" << engine->GetIndex()
            << ") Platform: Clipboard.hasStrings\n\ttext: \""
            << g_clipboard
            << "\"\n";

        rapidjson::Value s;
        s = rapidjson::StringRef(g_clipboard.c_str());
        res.AddMember("text", s, allocator);
        result = codec.EncodeSuccessEnvelope(&res);
      }
    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  } else if (method == kMethodClipboardSetData) {
    if (!args->IsNull() && args->HasMember("text") &&
        !((*args)["text"].IsNull())) {
      FML_DLOG(INFO) << "(" << engine->GetIndex() << ") Clipboard Data Set: \n"
                     << (*args)["text"].GetString();
      g_clipboard.assign((*args)["text"].GetString());
      result = codec.EncodeSuccessEnvelope();
    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  } else if (method == kMethodSetEnabledSystemUIOverlays) {
    FML_DLOG(INFO) << "(" << engine->GetIndex()
                   << ") System UI Overlays Enabled";
    result = codec.EncodeSuccessEnvelope();
  } else if (method == kHapticFeedbackVibrate) {
    FML_DLOG(INFO) << "(" << engine->GetIndex()
                   << ") Haptic Feedback - Vibrate";
    result = codec.EncodeSuccessEnvelope();
  } else if (method == kMethodSetSystemUiOverlayStyle) {
    SystemUiOverlayStyle style{};
    if (args->HasMember(kSystemNavigationBarColor) &&
        !(*args)[kSystemNavigationBarColor].IsNull() &&
        (*args)[kSystemNavigationBarColor].IsNumber()) {
      style.systemNavigationBarColor =
          (*args)[kSystemNavigationBarColor].GetUint();
      FML_DLOG(INFO) << "(" << engine->GetIndex() << ") "
                     << kSystemNavigationBarColor << ": "
                     << style.systemNavigationBarColor;
    }
    if (args->HasMember(kSystemNavigationBarDividerColor) &&
        !(*args)[kSystemNavigationBarDividerColor].IsNull() &&
        (*args)[kSystemNavigationBarDividerColor].IsNumber()) {
      style.systemNavigationBarDividerColor =
          (*args)[kSystemNavigationBarDividerColor].GetUint();
      FML_DLOG(INFO) << "(" << engine->GetIndex() << ") "
                     << kSystemNavigationBarDividerColor << ": "
                     << style.systemNavigationBarDividerColor;
    }
    if (args->HasMember(kSystemStatusBarContrastEnforced) &&
        !(*args)[kSystemStatusBarContrastEnforced].IsNull() &&
        (*args)[kSystemStatusBarContrastEnforced].IsBool()) {
      style.systemStatusBarContrastEnforced =
          (*args)[kSystemStatusBarContrastEnforced].GetBool();
      FML_DLOG(INFO) << "(" << engine->GetIndex() << ") "
                     << kSystemStatusBarContrastEnforced << ": "
                     << style.systemStatusBarContrastEnforced;
    }
    if (args->HasMember(kStatusBarColor) &&
        !(*args)[kStatusBarColor].IsNull() &&
        (*args)[kStatusBarColor].IsNumber()) {
      style.statusBarColor = (*args)[kStatusBarColor].GetUint();
      FML_DLOG(INFO) << "(" << engine->GetIndex() << ") " << kStatusBarColor
                     << ": " << style.statusBarColor;
    }
    if (args->HasMember(kStatusBarBrightness) &&
        !(*args)[kStatusBarBrightness].IsNull() &&
        (*args)[kStatusBarBrightness].IsString()) {
      style.statusBarBrightness = (*args)[kStatusBarBrightness].GetString();
      FML_DLOG(INFO) << "(" << engine->GetIndex() << ") "
                     << kStatusBarBrightness << ": "
                     << style.statusBarBrightness;
    }
    if (args->HasMember(kStatusBarIconBrightness) &&
        !(*args)[kStatusBarIconBrightness].IsNull() &&
        (*args)[kStatusBarIconBrightness].IsString()) {
      style.statusBarIconBrightness =
          (*args)[kStatusBarIconBrightness].GetString();
      FML_DLOG(INFO) << "(" << engine->GetIndex() << ") "
                     << kStatusBarIconBrightness << ": "
                     << style.statusBarIconBrightness;
    }
    if (args->HasMember(kSystemNavigationBarIconBrightness) &&
        !(*args)[kSystemNavigationBarIconBrightness].IsNull() &&
        (*args)[kSystemNavigationBarIconBrightness].IsString()) {
      style.systemNavigationBarIconBrightness =
          (*args)[kSystemNavigationBarIconBrightness].GetString();
      FML_DLOG(INFO) << "(" << engine->GetIndex() << ") "
                     << kSystemNavigationBarIconBrightness << ": "
                     << style.systemNavigationBarIconBrightness;
    }
    if (args->HasMember(kSystemNavigationBarContrastEnforced) &&
        !(*args)[kSystemNavigationBarContrastEnforced].IsNull() &&
        (*args)[kSystemNavigationBarContrastEnforced].IsBool()) {
      style.systemNavigationBarContrastEnforced =
          (*args)[kSystemNavigationBarContrastEnforced].GetBool();
      FML_DLOG(INFO) << "(" << engine->GetIndex() << ") "
                     << kSystemNavigationBarContrastEnforced << ": "
                     << style.systemNavigationBarContrastEnforced;
    }
    result = codec.EncodeSuccessEnvelope();
  } else {
    FML_DLOG(ERROR) << "(" << engine->GetIndex() << ") Platform: " << method
                    << " is unhandled";
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
