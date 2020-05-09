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

#include <cstring>

#include <flutter/fml/logging.h>
#include <flutter/json_method_codec.h>

#include "engine.h"
#include "hexdump.h"

constexpr char kMethod[] = "method";
constexpr char kArgs[] = "args";

static void PrintMessageAsHexDump(const FlutterPlatformMessage* msg) {
  std::stringstream ss;
  ss << Hexdump(msg->message, msg->message_size);
  FML_DLOG(INFO) << "Channel: \"" << msg->channel << "\"\n" << ss.str();
}

void Platform::OnPlatformMessage(const FlutterPlatformMessage* message,
                                 void* userdata) {
  auto engine = reinterpret_cast<Engine*>(userdata);
  rapidjson::Document document;
  document.Parse(reinterpret_cast<const char*>(message->message),
                 message->message_size);
  if (document.HasParseError() || !document.IsObject()) {
    return;
  }

  constexpr char kMethodSetApplicationSwitcherDescription[] =
      "SystemChrome.setApplicationSwitcherDescription";
  constexpr char kMethodSetSystemUiOverlayStyle[] =
      "SystemChrome.setSystemUIOverlayStyle";
  constexpr char kMethodSetEnabledSystemUIOverlays[] =
      "SystemChrome.setEnabledSystemUIOverlays";
  //  constexpr char kMethodGetClipboardDataMethod[] = "Clipboard.getData";
  //  constexpr char kMethodSetClipboardDataMethod[] = "Clipboard.setData";
  //  constexpr char kMethodSystemNavigatorPopMethod[] = "SystemNavigator.pop";
  //  constexpr char kMethodSystemSoundPlay[] = "SystemSound.play";
  //  constexpr char kMethodTextKey[] = "text";

  // PrintMessageAsHexDump(message);

  const char* method = nullptr;
  if (document.HasMember(kMethod) && document[kMethod].IsString()) {
    method = document[kMethod].GetString();

    if (document.HasMember(kArgs) &&
        (document[kArgs].IsObject() || document[kArgs].IsString() ||
         document[kArgs].IsArray())) {
#if 0
      if (0 == strcmp(kMethodGetClipboardDataMethod, method)) {
        auto format = document[kArgs].GetString();

        FML_DLOG(INFO) << "Clipboard.getData: format: " << format;

        rapidjson::Document d;
        d.SetObject();

        rapidjson::Value clipboard;

        auto data = engine->GetClipboardData();

        clipboard.SetString(data.c_str(),
                            static_cast<rapidjson::SizeType>(data.size()),
                            d.GetAllocator());

        engine->SendPlatformMessageResponse(
            message->response_handle,
            reinterpret_cast<const uint8_t*>(d.GetString()),
            d.GetStringLength());
        return;
      } else if (0 == strcmp(kMethodSetClipboardDataMethod, method)) {
        auto args = document[kArgs].GetString();

        FML_DLOG(INFO) << "Clipboard.setData: args=" << args;
      } else
#endif
      if (0 == strcmp(kMethodSetEnabledSystemUIOverlays, method)) {
        FML_DLOG(INFO) << "SystemChrome.setEnabledSystemUIOverlays";
      } else if (0 ==
                 strcmp(kMethodSetApplicationSwitcherDescription, method)) {
        unsigned int primaryColor;
        auto args = document[kArgs].GetObject();
        if (args.HasMember("primaryColor") && args["primaryColor"].IsNumber()) {
          primaryColor = args["primaryColor"].GetUint();
        }
        FML_DLOG(INFO) << "setApplicationSwitcherDescription: primaryColor: "
                       << primaryColor;
      } else if (0 == strcmp(kMethodSetSystemUiOverlayStyle, method)) {
        unsigned int systemNavigationBarColor = 0;
        unsigned int systemNavigationBarDividerColor = 0;
        unsigned int statusBarColor = 0;
        std::string statusBarBrightness, statusBarIconBrightness,
            systemNavigationBarIconBrightness;
        auto args = document[kArgs].GetObject();
        if (args.HasMember("systemNavigationBarColor") &&
            !args["systemNavigationBarColor"].IsNull() &&
            args["systemNavigationBarColor"].IsNumber()) {
          systemNavigationBarColor = args["systemNavigationBarColor"].GetUint();
        }
        if (args.HasMember("systemNavigationBarDividerColor") &&
            !args["systemNavigationBarDividerColor"].IsNull() &&
            args["systemNavigationBarDividerColor"].IsNumber()) {
          systemNavigationBarDividerColor =
              args["systemNavigationBarDividerColor"].GetUint();
        }
        if (args.HasMember("statusBarColor") &&
            !args["statusBarColor"].IsNull() &&
            args["statusBarColor"].IsNumber()) {
          statusBarColor = args["statusBarColor"].GetUint();
        }
        if (args.HasMember("statusBarBrightness") &&
            !args["statusBarBrightness"].IsNull() &&
            args["statusBarBrightness"].IsString()) {
          statusBarBrightness = args["statusBarBrightness"].GetString();
        }
        if (args.HasMember("statusBarIconBrightness") &&
            !args["statusBarIconBrightness"].IsNull() &&
            args["statusBarIconBrightness"].IsString()) {
          statusBarIconBrightness = args["statusBarIconBrightness"].GetString();
        }
        if (args.HasMember("systemNavigationBarIconBrightness") &&
            !args["systemNavigationBarIconBrightness"].IsNull() &&
            args["systemNavigationBarIconBrightness"].IsString()) {
          systemNavigationBarIconBrightness =
              args["systemNavigationBarIconBrightness"].GetString();
        }
        FML_DLOG(INFO) << "** setSystemUIOverlayStyle **\n"
                          "\tsystemNavigationBarColor="
                       << systemNavigationBarColor
                       << "\n"
                          "\tsystemNavigationBarDividerColor="
                       << systemNavigationBarDividerColor
                       << "\n"
                          "\tstatusBarColor="
                       << statusBarColor
                       << "\n"
                          "\tstatusBarBrightness="
                       << statusBarBrightness
                       << "\n"
                          "\tstatusBarIconBrightness="
                       << statusBarIconBrightness
                       << "\n"
                          "\tsystemNavigationBarIconBrightness="
                       << systemNavigationBarIconBrightness;
      }
    }
  } else {
    FML_LOG(ERROR) << "Platform Unhandled: " << method;
    PrintMessageAsHexDump(message);
  }

  engine->SendPlatformMessageResponse(message->response_handle, nullptr, 0);
}