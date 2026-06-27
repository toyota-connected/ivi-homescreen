// Copyright 2020-2024 Toyota Connected North America
// @copyright Copyright (c) 2022 Woven Alpha, Inc.
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

#include "mouse_cursor_handler.h"

#include <flutter/standard_method_codec.h>

#include "display/idisplay.h"
#include "engine.h"
#include "view/flutter_view.h"

static constexpr char kNoViewError[] = "Missing view error";

MouseCursorHandler::MouseCursorHandler(flutter::BinaryMessenger* messenger,
                                       FlutterView* view)
    : channel_(std::make_unique<flutter::MethodChannel<>>(
          messenger,
          "flutter/mousecursor",
          &flutter::StandardMethodCodec::GetInstance())),
      view_(view) {
  channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<flutter::EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                 result) { HandleMethodCall(call, std::move(result)); });
}

void MouseCursorHandler::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
    const {
  const std::string& method = method_call.method_name();

  if (method == "activateSystemCursor") {
    const auto& args =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    int32_t device = 0;
    std::string kind;
    for (auto& it : *args) {
      if ("device" == std::get<std::string>(it.first) &&
          std::holds_alternative<int32_t>(it.second)) {
        device = std::get<int32_t>(it.second);
      }
      if ("kind" == std::get<std::string>(it.first) &&
          std::holds_alternative<std::string>(it.second)) {
        kind = std::get<std::string>(it.second);
      }
    }
    if (!view_) {
      result->Error(kNoViewError, "View is not set.");
      return;
    }
    // Route through the backend-agnostic IDisplay: every backend (Wayland
    // Display, DrmDisplay, SoftwareDisplay) implements ActivateSystemCursor.
    // The Wayland path used to hop through WaylandWindow, which merely
    // forwarded to this same display call — going direct drops the
    // Wayland-only gate so DRM and software receive the cursor kind too.
    const bool res = view_->GetIDisplay()->ActivateSystemCursor(device, kind);
    result->Success(flutter::EncodableValue(res));
  } else {
    result->NotImplemented();
  }
}
