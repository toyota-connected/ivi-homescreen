// Copyright 2020 Toyota Connected North America
// @copyright Copyright (c) 2022 Woven Alpha, Inc.
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

#include <flutter/shell/platform/common/json_method_codec.h>

#include "engine.h"
#include "logging.h"

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
      SPDLOG_DEBUG(
          "({}) Platform: ApplicationSwitcherDescription\n\tlabel: "
          "\"{}\"\n\tprimaryColor: {}",
          engine->GetIndex(), description.label, description.primaryColor);

      result = codec.EncodeSuccessEnvelope();
    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  } else if (method == kPlaySoundMethod) {
    if (!args->IsNull() && args->IsString()) {
      auto sound_type = args->GetString();
      if (0 == strcmp(sound_type, kSoundTypeClick)) {
        SPDLOG_DEBUG("({}) SystemSound.play ** click **", engine->GetIndex());
        result = codec.EncodeSuccessEnvelope();
      } else if (0 == strcmp(sound_type, kSoundTypeAlert)) {
        SPDLOG_DEBUG("({}) SystemSound.play ** alert **", engine->GetIndex());
        result = codec.EncodeSuccessEnvelope();
      }
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

        if (g_clipboard.empty()) {
          res.AddMember("value", false, allocator);
        } else {
          res.AddMember("value", true, allocator);
        }
        result = codec.EncodeSuccessEnvelope(&res);
      }
    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  } else if (method == kMethodClipboardGetData) {
    if (!args->IsNull() && args->IsString()) {
      auto format = args->GetString();
      if (0 == strcmp(format, kTextPlainFormat)) {
        rapidjson::Document res;
        res.SetObject();
        auto& allocator = res.GetAllocator();
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
      SPDLOG_DEBUG("({}) Clipboard Data Set: \n{}", engine->GetIndex(),
                   (*args)["text"].GetString());
      g_clipboard.assign((*args)["text"].GetString());
      result = codec.EncodeSuccessEnvelope();
    } else {
      result = codec.EncodeErrorEnvelope("argument_error", "Invalid Arguments");
    }
  } else if (method == kMethodSetEnabledSystemUIOverlays) {
    SPDLOG_DEBUG("({}) System UI Overlays Enabled", engine->GetIndex());
    result = codec.EncodeSuccessEnvelope();
  } else if (method == kHapticFeedbackVibrate) {
    SPDLOG_DEBUG("({}) Haptic Feedback - Vibrate", engine->GetIndex());
    result = codec.EncodeSuccessEnvelope();
  } else if (method == kSystemChrome_setPreferredOrientations) {
    SPDLOG_DEBUG("({}) setPreferredOrientations", engine->GetIndex());
    if (!args->IsNull()) {
      for (const auto& field : args->GetArray()) {
        (void)field;
        SPDLOG_DEBUG("({}) {}", engine->GetIndex(), field.GetString());
      }
    }
    result = codec.EncodeSuccessEnvelope();
  } else if (method == kSystemChrome_setEnabledSystemUIMode) {
    if (!args->IsNull() && args->IsString()) {
      SPDLOG_DEBUG("({}) SystemChrome.setEnabledSystemUIMode: {}",
                   engine->GetIndex(), args->GetString());
    } else {
      SPDLOG_DEBUG("({}) SystemChrome.setEnabledSystemUIMode: Unknown",
                   engine->GetIndex());
    }
    result = codec.EncodeSuccessEnvelope();
  } else if (method == kMethodSetSystemUiOverlayStyle) {
    SystemUiOverlayStyle style{};
    if (args->HasMember(kSystemNavigationBarColor) &&
        !(*args)[kSystemNavigationBarColor].IsNull() &&
        (*args)[kSystemNavigationBarColor].IsNumber()) {
      style.systemNavigationBarColor =
          (*args)[kSystemNavigationBarColor].GetUint();
      SPDLOG_DEBUG("({}) {}: {}", engine->GetIndex(), kSystemNavigationBarColor,
                   style.systemNavigationBarColor);
    }
    if (args->HasMember(kSystemNavigationBarDividerColor) &&
        !(*args)[kSystemNavigationBarDividerColor].IsNull() &&
        (*args)[kSystemNavigationBarDividerColor].IsNumber()) {
      style.systemNavigationBarDividerColor =
          (*args)[kSystemNavigationBarDividerColor].GetUint();
      SPDLOG_DEBUG("({}) {}: {}", engine->GetIndex(),
                   kSystemNavigationBarDividerColor,
                   style.systemNavigationBarDividerColor);
    }
    if (args->HasMember(kSystemStatusBarContrastEnforced) &&
        !(*args)[kSystemStatusBarContrastEnforced].IsNull() &&
        (*args)[kSystemStatusBarContrastEnforced].IsBool()) {
      style.systemStatusBarContrastEnforced =
          (*args)[kSystemStatusBarContrastEnforced].GetBool();
      SPDLOG_DEBUG("({}) {}: {}", engine->GetIndex(),
                   kSystemStatusBarContrastEnforced,
                   style.systemStatusBarContrastEnforced);
    }
    if (args->HasMember(kStatusBarColor) &&
        !(*args)[kStatusBarColor].IsNull() &&
        (*args)[kStatusBarColor].IsNumber()) {
      style.statusBarColor = (*args)[kStatusBarColor].GetUint();
      SPDLOG_DEBUG("({}) {}:{}", engine->GetIndex(), kStatusBarColor,
                   style.statusBarColor);
    }
    if (args->HasMember(kStatusBarBrightness) &&
        !(*args)[kStatusBarBrightness].IsNull() &&
        (*args)[kStatusBarBrightness].IsString()) {
      style.statusBarBrightness = (*args)[kStatusBarBrightness].GetString();
      SPDLOG_DEBUG("({}) {}:{}", engine->GetIndex(), kStatusBarBrightness,
                   style.statusBarBrightness);
    }
    if (args->HasMember(kStatusBarIconBrightness) &&
        !(*args)[kStatusBarIconBrightness].IsNull() &&
        (*args)[kStatusBarIconBrightness].IsString()) {
      style.statusBarIconBrightness =
          (*args)[kStatusBarIconBrightness].GetString();
      SPDLOG_DEBUG("({}) {}:{}", engine->GetIndex(), kStatusBarIconBrightness,
                   style.statusBarIconBrightness);
    }
    if (args->HasMember(kSystemNavigationBarIconBrightness) &&
        !(*args)[kSystemNavigationBarIconBrightness].IsNull() &&
        (*args)[kSystemNavigationBarIconBrightness].IsString()) {
      style.systemNavigationBarIconBrightness =
          (*args)[kSystemNavigationBarIconBrightness].GetString();
      SPDLOG_DEBUG("({}) {}:{}", engine->GetIndex(),
                   kSystemNavigationBarIconBrightness,
                   style.systemNavigationBarIconBrightness);
    }
    if (args->HasMember(kSystemNavigationBarContrastEnforced) &&
        !(*args)[kSystemNavigationBarContrastEnforced].IsNull() &&
        (*args)[kSystemNavigationBarContrastEnforced].IsBool()) {
      style.systemNavigationBarContrastEnforced =
          (*args)[kSystemNavigationBarContrastEnforced].GetBool();
      SPDLOG_DEBUG("({}) {}:{}", engine->GetIndex(),
                   kSystemNavigationBarContrastEnforced,
                   style.systemNavigationBarContrastEnforced);
    }
    result = codec.EncodeSuccessEnvelope();
  } else if (method == kLiveText_isLiveTextInputAvailable) {
    SPDLOG_DEBUG("({}) {}", engine->GetIndex(), kLiveText_isLiveTextInputAvailable);
    result = codec.EncodeSuccessEnvelope();
  } else if (method == kSystemInitializationComplete) {
    SPDLOG_DEBUG("({}) {}", engine->GetIndex(), kSystemInitializationComplete);
    result = codec.EncodeSuccessEnvelope();
  } else {
    spdlog::error("({}) Platform: {} is unhandled", engine->GetIndex(), method);
    result = codec.EncodeErrorEnvelope("unhandled_method", "Unhandled Method");
  }

  engine->SendPlatformMessageResponse(message->response_handle, result->data(),
                                      result->size());
}
