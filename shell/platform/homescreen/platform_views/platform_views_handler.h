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

#include <map>

#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/method_call.h>
#include <flutter/method_channel.h>
#include <flutter/method_result.h>

#include "flutter_homescreen.h"

#include <flutter/standard_method_codec.h>

class FlutterView;

class PlatformViewsHandler {
 public:
  explicit PlatformViewsHandler(flutter::BinaryMessenger* messenger,
                                FlutterDesktopEngineRef engine);

 private:
  // Called when a method is called on |channel_|;
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  static void PlatformViewAddListener(
      void* context,
      int32_t id,
      const struct platform_view_listener* listener,
      void* listener_context);

  static void PlatformViewRemoveListener(void* context, int32_t id);

  // The MethodChannel used for communication with the Flutter engine.
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;

  // A reference to the opaque data pointer, if any. Null in headless mode.
  FlutterDesktopEngineRef engine_;

  std::map<int32_t, std::pair<const struct platform_view_listener*, void*>>
      listeners_;
};
