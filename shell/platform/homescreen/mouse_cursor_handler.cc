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

#include "engine.h"

// Complete WaylandWindow type required at line ~69 for the
// window->ActivateSystemCursor() call. Forward-decl in flutter_view.h
// is enough for everything else (GetWindow's return type, the
// `if (!window)` null check).
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN || \
    BUILD_BACKEND_HEADLESS_EGL
#include "wayland/window.h"
#endif

static constexpr char kNoWindowError[] = "Missing window error";

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
      result->Error(kNoWindowError, "View is not set.");
      return;
    }
    auto window = view_->GetWindow();
    if (!window) {
      // DRM/KMS + software paths have no WaylandWindow; cursor is
      // handled at the KMS plane level (or not at all). Same
      // behaviour on non-Wayland builds where GetWindow() is
      // always null and WaylandWindow is forward-declared only.
      result->Success(flutter::EncodableValue(true));
      return;
    }
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN || \
    BUILD_BACKEND_HEADLESS_EGL
    auto res = window->ActivateSystemCursor(device, kind);
    result->Success(flutter::EncodableValue(res));
#else
    // Unreachable — window is always null without a Wayland backend
    // (see the early return above). The branch exists only so the
    // compiler doesn't need WaylandWindow's complete type here.
    (void)device;
    (void)kind;
    result->Success(flutter::EncodableValue(true));
#endif
  } else {
    result->NotImplemented();
  }
}
