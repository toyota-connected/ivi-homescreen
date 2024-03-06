/*
 * Copyright 2020-2024 Toyota Connected North America
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

#undef _HAS_EXCEPTIONS

#include "messages.h"

#include <flutter/basic_message_channel.h>
#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/method_call.h>
#include <flutter/method_channel.h>

#include <optional>
#include <string>

#include "plugins/common/common.h"

namespace nav_render_view_plugin {

using flutter::BasicMessageChannel;
using flutter::CustomEncodableValue;
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;
using flutter::MethodCall;
using flutter::MethodResult;

/// The codec used by NavRenderViewApi.
const flutter::StandardMethodCodec& NavRenderViewApi::GetCodec() {
  return flutter::StandardMethodCodec::GetInstance();
}

// Sets up an instance of `NavRenderViewApi` to handle messages through the
// `binary_messenger`.
void NavRenderViewApi::SetUp(flutter::BinaryMessenger* binary_messenger,
                             NavRenderViewApi* api) {
  {
    auto channel = std::make_unique<flutter::MethodChannel<>>(
        binary_messenger, "nav_render_view", &GetCodec());
    if (api != nullptr) {
      channel->SetMethodCallHandler(
          [api](const flutter::MethodCall<>& call,
                std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                    result) { result->NotImplemented(); });
    } else {
      channel->SetMethodCallHandler(nullptr);
    }
  }
}

}  // namespace nav_render_view_plugin
