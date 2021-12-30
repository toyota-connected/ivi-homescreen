/*
 * Copyright 2020 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <flutter_embedder.h>
#include <string>

constexpr char kChannelPlatform[] = "flutter/platform";

class Platform {
 public:
  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);

  struct MethodSetApplicationSwitcherDescription {
    std::string label;
    uint32_t primaryColor;
  };

  struct SystemUiOverlayStyle {
    unsigned int systemNavigationBarColor;
    unsigned int systemNavigationBarDividerColor;
    unsigned int statusBarColor;
    std::string statusBarBrightness;
    std::string statusBarIconBrightness;
    std::string systemNavigationBarIconBrightness;
  };

 private:
  static constexpr char kMethodSetApplicationSwitcherDescription[] =
      "SystemChrome.setApplicationSwitcherDescription";

  static constexpr char kMethodSetSystemUiOverlayStyle[] =
      "SystemChrome.setSystemUIOverlayStyle";
  static constexpr char kMethodSetEnabledSystemUIOverlays[] =
      "SystemChrome.setEnabledSystemUIOverlays";
#if 0
  static constexpr char kMethodSystemNavigatorPopMethod[] = "SystemNavigator.pop";

  static constexpr char kBadArgumentsError[] = "Bad Arguments";
  static constexpr char kUnknownClipboardFormatError[] = "Unknown Clipboard Format";
  static constexpr char kFailedError[] = "Failed";
#endif
  static constexpr char kMethodClipboardHasStrings[] = "Clipboard.hasStrings";
  static constexpr char kMethodClipboardSetData[] = "Clipboard.setData";
#if 0
  static constexpr char kGetClipboardDataMethod[] = "Clipboard.getData";
  static constexpr char kSystemNavigatorPopMethod[] = "SystemNavigator.pop";
#endif
  static constexpr char kTextPlainFormat[] = "text/plain";

#if 0
  static constexpr char kPlaySoundMethod[] = "SystemSound.play";
  static constexpr char kSoundTypeAlert[] = "SystemSoundType.alert";
  static constexpr char kSoundTypeClick[] = "SystemSoundType.click";
#endif
};
