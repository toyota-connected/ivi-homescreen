/*
 * Copyright 2020-2023 Toyota Connected North America
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

#include <shell/platform/embedder/embedder.h>

class PlatformViews {
 public:
  static constexpr char kChannelName[] = "flutter/platform_views";

  /**
   * @brief Callback function for platform messages about platform view
   * @param[in] message Receive message
   * @param[in] userdata Pointer to User data
   * @return void
   * @relation
   * flutter
   */
  static void OnPlatformMessage(const FlutterPlatformMessage* message,
                                void* userdata);

  PlatformViews(PlatformViews& other) = delete;

  void operator=(const PlatformViews&) = delete;

 private:
  static constexpr char kMethodCreate[] = "create";
  static constexpr char kMethodDispose[] = "dispose";
  static constexpr char kMethodResize[] = "resize";
  static constexpr char kMethodSetDirection[] = "setDirection";
  static constexpr char kMethodClearFocus[] = "clearFocus";
  static constexpr char kMethodOffset[] = "offset";
  static constexpr char kMethodTouch[] = "touch";

  static constexpr char kKeyId[] = "id";
  static constexpr char kKeyViewType[] = "viewType";
  static constexpr char kKeyDirection[] = "direction";
  static constexpr char kKeyWidth[] = "width";
  static constexpr char kKeyHeight[] = "height";
  static constexpr char kKeyParams[] = "params";
  static constexpr char kKeyTop[] = "top";
  static constexpr char kKeyLeft[] = "left";
  static constexpr char kKeyHybrid[] = "hybrid";
};
