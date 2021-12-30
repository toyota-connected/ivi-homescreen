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
#include <flutter/json_method_codec.h>

#include "engine.h"

void Platform::OnPlatformMessage(const FlutterPlatformMessage* message,
                                 void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  auto codec = &flutter::JsonMethodCodec::GetInstance();
  auto obj = codec->DecodeMethodCall(message->message, message->message_size);
  auto method = obj->method_name();
  auto args = obj->arguments();

  if (method == kMethodSetApplicationSwitcherDescription) {
    if (args->HasMember("label") && args->HasMember("primaryColor")) {
      MethodSetApplicationSwitcherDescription description{};
      description.label = (*args)["label"].GetString();
      description.primaryColor = (*args)["primaryColor"].GetUint();
      FML_DLOG(INFO) << "Platform: ApplicationSwitcherDescription\n\tlabel: \""
                     << description.label
                     << "\"\n\tprimaryColor: " << description.primaryColor;
      auto res = codec->EncodeSuccessEnvelope();
      engine->SendPlatformMessageResponse(message->response_handle, res->data(),
                                          res->size());
      return;
    }
  } else if (method == kMethodClipboardHasStrings) {
    if (args->IsString()) {
      auto format = args->GetString();
      if (0 == strcmp(format, kTextPlainFormat)) {
        rapidjson::Document result;
        result.SetBool(false);
        auto res = codec->EncodeSuccessEnvelope(&result);
        engine->SendPlatformMessageResponse(message->response_handle,
                                            res->data(), res->size());
        return;
      }
    }
  } else if (method == kMethodClipboardSetData) {
    if (args->HasMember("text") && !((*args)["text"].IsNull())) {
      FML_DLOG(INFO) << "Clipboard Data Set: \n" << (*args)["text"].GetString();
    }
    auto res = codec->EncodeSuccessEnvelope();
    engine->SendPlatformMessageResponse(message->response_handle, res->data(),
                                        res->size());
    return;
  } else if (method == kMethodSetEnabledSystemUIOverlays) {
    FML_DLOG(INFO) << "System UI Overlays Enabled";
    auto res = codec->EncodeSuccessEnvelope();
    engine->SendPlatformMessageResponse(message->response_handle, res->data(),
                                        res->size());
    return;
  } else if (method == kMethodSetSystemUiOverlayStyle) {
    SystemUiOverlayStyle style{};
    if (args->HasMember("systemNavigationBarColor") &&
        !(*args)["systemNavigationBarColor"].IsNull() &&
        (*args)["systemNavigationBarColor"].IsNumber()) {
      style.systemNavigationBarColor =
          (*args)["systemNavigationBarColor"].GetUint();
    }
    if (args->HasMember("systemNavigationBarDividerColor") &&
        !(*args)["systemNavigationBarDividerColor"].IsNull() &&
        (*args)["systemNavigationBarDividerColor"].IsNumber()) {
      style.systemNavigationBarDividerColor =
          (*args)["systemNavigationBarDividerColor"].GetUint();
    }
    if (args->HasMember("statusBarColor") &&
        !(*args)["statusBarColor"].IsNull() &&
        (*args)["statusBarColor"].IsNumber()) {
      style.statusBarColor = (*args)["statusBarColor"].GetUint();
    }
    if (args->HasMember("statusBarBrightness") &&
        !(*args)["statusBarBrightness"].IsNull() &&
        (*args)["statusBarBrightness"].IsString()) {
      style.statusBarBrightness = (*args)["statusBarBrightness"].GetString();
    }
    if (args->HasMember("statusBarIconBrightness") &&
        !(*args)["statusBarIconBrightness"].IsNull() &&
        (*args)["statusBarIconBrightness"].IsString()) {
      style.statusBarIconBrightness =
          (*args)["statusBarIconBrightness"].GetString();
    }
    if (args->HasMember("systemNavigationBarIconBrightness") &&
        !(*args)["systemNavigationBarIconBrightness"].IsNull() &&
        (*args)["systemNavigationBarIconBrightness"].IsString()) {
      style.systemNavigationBarIconBrightness =
          (*args)["systemNavigationBarIconBrightness"].GetString();
    }
    FML_DLOG(INFO) << "** System UI Overlays Style **"
                   << "\n\tsystemNavigationBarColor="
                   << style.systemNavigationBarColor
                   << "\n\tsystemNavigationBarDividerColor="
                   << style.systemNavigationBarDividerColor
                   << "\n\tstatusBarColor=" << style.statusBarColor
                   << "\n\tstatusBarBrightness=" << style.statusBarBrightness
                   << "\n\tstatusBarIconBrightness="
                   << style.statusBarIconBrightness
                   << "\n\tsystemNavigationBarIconBrightness="
                   << style.systemNavigationBarIconBrightness;
  }

  FML_DLOG(INFO) << "[Platform] Unhandled: " << method;
  engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
}
