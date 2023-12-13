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

#include "platform_handler.h"

#include <flutter/shell/platform/common/json_method_codec.h>

#include "engine.h"
#include "logging.h"

static constexpr char kMethodSetApplicationSwitcherDescription[] =
    "SystemChrome.setApplicationSwitcherDescription";

static constexpr char kMethodSetSystemUiOverlayStyle[] =
    "SystemChrome.setSystemUIOverlayStyle";
static constexpr char kMethodSetEnabledSystemUIOverlays[] =
    "SystemChrome.setEnabledSystemUIOverlays";
static constexpr char kHapticFeedbackVibrate[] = "HapticFeedback.vibrate";
#if 0
    static constexpr char kBadArgumentsError[] = "Bad Arguments";
    static constexpr char kUnknownClipboardFormatError[] = "Unknown Clipboard Format";
    static constexpr char kFailedError[] = "Failed";
#endif
static constexpr char kMethodClipboardHasStrings[] = "Clipboard.hasStrings";
static constexpr char kMethodClipboardGetData[] = "Clipboard.getData";
static constexpr char kMethodClipboardSetData[] = "Clipboard.setData";
#if 0
    static constexpr char kGetClipboardDataMethod[] = "Clipboard.getData";
#endif
static constexpr char kTextPlainFormat[] = "text/plain";
static constexpr char kTextKey[] = "text";

static constexpr char kPlaySoundMethod[] = "SystemSound.play";
static constexpr char kSoundTypeAlert[] = "SystemSoundType.alert";
static constexpr char kSoundTypeClick[] = "SystemSoundType.click";

static constexpr char kSystemNavigationBarColor[] = "systemNavigationBarColor";
static constexpr char kSystemNavigationBarDividerColor[] =
    "systemNavigationBarDividerColor";
static constexpr char kSystemStatusBarContrastEnforced[] =
    "systemStatusBarContrastEnforced";
static constexpr char kStatusBarColor[] = "statusBarColor";
static constexpr char kStatusBarBrightness[] = "statusBarBrightness";
static constexpr char kStatusBarIconBrightness[] = "statusBarIconBrightness";
static constexpr char kSystemNavigationBarIconBrightness[] =
    "systemNavigationBarIconBrightness";
static constexpr char kSystemNavigationBarContrastEnforced[] =
    "systemNavigationBarContrastEnforced";
static constexpr char kSystemChrome_setPreferredOrientations[] =
    "SystemChrome.setPreferredOrientations";
static constexpr char kSystemChrome_setEnabledSystemUIMode[] =
    "SystemChrome.setEnabledSystemUIMode";

static constexpr char kSystemInitializationComplete[] =
    "System.initializationComplete";
static constexpr char kLiveText_isLiveTextInputAvailable[] =
    "LiveText.isLiveTextInputAvailable";

static constexpr char kNoWindowError[] = "Missing window error";
static constexpr char kUnknownClipboardFormatError[] =
    "Unknown clipboard format error";

std::string g_clipboard;

PlatformHandler::PlatformHandler(flutter::BinaryMessenger* messenger,
                                 FlutterView* view)
    : channel_(std::make_unique<flutter::MethodChannel<rapidjson::Document>>(
          messenger,
          "flutter/platform",
          &flutter::JsonMethodCodec::GetInstance())),
      view_(view) {
  channel_->SetMethodCallHandler(
      [this](
          const flutter::MethodCall<rapidjson::Document>& call,
          std::unique_ptr<flutter::MethodResult<rapidjson::Document>> result) {
        HandleMethodCall(call, std::move(result));
      });
}

void PlatformHandler::HandleMethodCall(
    const flutter::MethodCall<rapidjson::Document>& method_call,
    std::unique_ptr<flutter::MethodResult<rapidjson::Document>> result) const {
  const std::string& method = method_call.method_name();

  if (method == kMethodClipboardGetData) {
    if (!view_) {
      result->Error(kNoWindowError,
                    "Clipboard is not available in headless mode.");
      return;
    }
    // Only one string argument is expected.
    const rapidjson::Value& format = method_call.arguments()[0];

    if (strcmp(format.GetString(), kTextPlainFormat) != 0) {
      result->Error(kUnknownClipboardFormatError,
                    "Clipboard API only supports text.");
      return;
    }

    const char* clipboardData = "";  // TODO glfwGetClipboardString(window_);
    if (clipboardData == nullptr) {
      result->Error(kUnknownClipboardFormatError,
                    "Failed to retrieve clipboard data from GLFW api.");
      return;
    }
    rapidjson::Document document;
    document.SetObject();
    rapidjson::Document::AllocatorType& allocator = document.GetAllocator();
    document.AddMember(rapidjson::Value(kTextKey, allocator),
                       rapidjson::Value(clipboardData, allocator), allocator);
    result->Success(document);
  } else if (method == kMethodClipboardSetData) {
    if (!view_) {
      result->Error(kNoWindowError,
                    "Clipboard is not available in headless mode.");
      return;
    }
    const rapidjson::Value& document = *method_call.arguments();
    const rapidjson::Value::ConstMemberIterator itr =
        document.FindMember(kTextKey);
    if (itr == document.MemberEnd()) {
      result->Error(kUnknownClipboardFormatError,
                    "Missing text to store on clipboard.");
      return;
    }
    //    glfwSetClipboardString(window_, itr->value.GetString());
    result->Success();
  } else {
    result->NotImplemented();
  }
}

#if 0
void Platform::OnPlatformMessage(const FlutterPlatformMessage* message,
                                 void* userdata) {
  std::unique_ptr<std::vector<uint8_t>> result;
  auto engine = static_cast<Engine*>(userdata);
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
#endif