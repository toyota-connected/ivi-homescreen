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

class UrlLauncher {
 public:
  static constexpr char kChannelName[] =
      "plugins.flutter.io/url_launcher_linux";
  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);

 private:
  static constexpr char kBadArgumentsError[] = "Bad Arguments";
  static constexpr char kLaunchError[] = "Launch Error";
  static constexpr char kCanLaunchMethod[] = "canLaunch";
  static constexpr char kLaunchMethod[] = "launch";
  static constexpr char kUrlKey[] = "url";
};
