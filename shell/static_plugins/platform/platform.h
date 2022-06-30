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

class Platform {
 public:
  static constexpr char kChannelName[] = "flutter/platform";
  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);

  struct MethodSetApplicationSwitcherDescription {
    std::string label;
    uint32_t primaryColor;
  };

  struct SystemUiOverlayStyle {
    /// The color of the system bottom navigation bar.
    ///
    /// Only honored in Android versions O and greater.
    uint32_t systemNavigationBarColor;

    /// The color of the divider between the system's bottom navigation bar and
    /// the app's content.
    ///
    /// Only honored in Android versions P and greater.
    uint32_t systemNavigationBarDividerColor;

    /// Overrides the contrast enforcement when setting a transparent navigation
    /// bar.
    ///
    /// When setting a transparent navigation bar in SDK 29+, or Android 10 and
    /// up, a translucent body scrim may be applied behind the button navigation
    /// bar to ensure contrast with buttons and the background of the
    /// application.
    ///
    /// SDK 28-, or Android P and lower, will not apply this body scrim.
    ///
    /// Setting this to false overrides the default body scrim.
    ///
    /// See also:
    ///
    ///   * [SystemUiOverlayStyle.systemNavigationBarColor], which is overridden
    ///   when transparent to enforce this contrast policy.
    bool systemStatusBarContrastEnforced;

    /// The color of top status bar.
    ///
    /// Only honored in Android version M and greater.
    uint32_t statusBarColor;

    /// The brightness of top status bar.
    ///
    /// Only honored in iOS.
    std::string statusBarBrightness;

    /// The brightness of the top status bar icons.
    ///
    /// Only honored in Android version M and greater.
    std::string statusBarIconBrightness;

    /// The brightness of the system navigation bar icons.
    ///
    /// Only honored in Android versions O and greater.
    /// When set to [Brightness.light], the system navigation bar icons are
    /// light. When set to [Brightness.dark], the system navigation bar icons
    /// are dark.
    std::string systemNavigationBarIconBrightness;

    /// Overrides the contrast enforcement when setting a transparent navigation
    /// bar.
    ///
    /// When setting a transparent navigation bar in SDK 29+, or Android 10 and
    /// up, a translucent body scrim may be applied behind the button navigation
    /// bar to ensure contrast with buttons and the background of the
    /// application.
    ///
    /// SDK 28-, or Android P and lower, will not apply this body scrim.
    ///
    /// Setting this to false overrides the default body scrim.
    ///
    /// See also:
    ///
    ///   * [SystemUiOverlayStyle.systemNavigationBarColor], which is overridden
    ///   when transparent to enforce this contrast policy.
    bool systemNavigationBarContrastEnforced;
  };

 private:
  static constexpr char kMethodSetApplicationSwitcherDescription[] =
      "SystemChrome.setApplicationSwitcherDescription";

  static constexpr char kMethodSetSystemUiOverlayStyle[] =
      "SystemChrome.setSystemUIOverlayStyle";
  static constexpr char kMethodSetEnabledSystemUIOverlays[] =
      "SystemChrome.setEnabledSystemUIOverlays";
  static constexpr char kHapticFeedbackVibrate[] = "HapticFeedback.vibrate";
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

  static constexpr char kSystemNavigationBarColor[] =
      "systemNavigationBarColor";
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
};
