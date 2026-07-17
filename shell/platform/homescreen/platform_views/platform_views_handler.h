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

#include <memory>

#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/method_call.h>
#include <flutter/method_channel.h>
#include <flutter/method_result.h>
#include <flutter/standard_method_codec.h>

#include "flutter_homescreen.h"
#include "platform_view_listener.h"

class PlatformViewRegistry;

// Thin adapter over the flutter/platform_views method channel: it decodes each
// message and forwards to the PlatformViewRegistry, which owns the id->instance
// lifecycle. It holds no view state of its own.
class PlatformViewsHandler {
 public:
  explicit PlatformViewsHandler(flutter::BinaryMessenger* messenger,
                                FlutterDesktopEngineRef engine);

 private:
  // Called when a method is invoked on |channel_|.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

  // Trampolines handed to the generated create dispatch. |context| is the
  // PlatformViewRegistry; a plugin also stores it to call the remove trampoline
  // from its destructor.
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

  // Owns the id->instance lifecycle; borrowed from the engine state.
  PlatformViewRegistry* registry_;
};
