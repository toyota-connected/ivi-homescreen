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

#include <binary_messenger.h>
#include <method_call.h>
#include <method_channel.h>
#include <method_result.h>

#include <string>

class FlutterView;

class MouseCursorHandler {
 public:
  explicit MouseCursorHandler(flutter::BinaryMessenger* messenger,
                              FlutterView* view);

 private:
  // Called when a method is called on |channel_|;
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue>& method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
      const;

  // The MethodChannel used for communication with the Flutter engine.
  std::unique_ptr<flutter::MethodChannel<>> channel_;

  // A reference to the opaque data pointer, if any. Null in headless mode.
  FlutterView* view_;
};
