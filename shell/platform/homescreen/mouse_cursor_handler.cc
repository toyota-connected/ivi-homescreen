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

#include <optional>
#include <vector>

#include "display/idisplay.h"
#include "engine.h"
#include "view/flutter_view.h"

static constexpr char kNoViewError[] = "Missing view error";
static constexpr char kBadArgumentsError[] = "Bad arguments";
static constexpr char kCustomCursorChannel[] = "ivi-homescreen/mouse_cursor";

namespace {
// The value under |key|, when it is an int of either width.
//
// The standard codec writes a small integer as int32 and a large one as
// int64, and which one arrives depends on the value rather than on the Dart
// declaration -- so reading only int32 works until a hotspot is computed
// rather than written as a literal.
std::optional<int32_t> IntAt(const flutter::EncodableMap& map,
                             const char* key) {
  const auto it = map.find(flutter::EncodableValue(std::string(key)));
  if (it == map.end()) {
    return std::nullopt;
  }
  if (const auto* v32 = std::get_if<int32_t>(&it->second)) {
    return *v32;
  }
  if (const auto* v64 = std::get_if<int64_t>(&it->second)) {
    return static_cast<int32_t>(*v64);
  }
  return std::nullopt;
}
}  // namespace

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

  custom_channel_ = std::make_unique<flutter::MethodChannel<>>(
      messenger, kCustomCursorChannel,
      &flutter::StandardMethodCodec::GetInstance());
  custom_channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<flutter::EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                 result) { HandleCustomMethodCall(call, std::move(result)); });
}

void MouseCursorHandler::HandleCustomMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result)
    const {
  const std::string& method = method_call.method_name();

  if (!view_) {
    result->Error(kNoViewError, "View is not set.");
    return;
  }

  if (method == "setCustomCursor") {
    const auto* args =
        std::get_if<flutter::EncodableMap>(method_call.arguments());
    if (args == nullptr) {
      result->Error(kBadArgumentsError, "setCustomCursor wants a map.");
      return;
    }
    const auto it = args->find(flutter::EncodableValue(std::string("buffer")));
    const auto* pixels = it == args->end()
                             ? nullptr
                             : std::get_if<std::vector<uint8_t>>(&it->second);
    const std::optional<int32_t> width = IntAt(*args, "width");
    const std::optional<int32_t> height = IntAt(*args, "height");
    if (pixels == nullptr || !width || !height) {
      result->Error(kBadArgumentsError,
                    "setCustomCursor wants buffer, width and height.");
      return;
    }
    // The hotspot is optional and defaults to the top-left, which is what a
    // caller that has not thought about it means.
    const int32_t hotspot_x = IntAt(*args, "hotspotX").value_or(0);
    const int32_t hotspot_y = IntAt(*args, "hotspotY").value_or(0);
    const int32_t device = IntAt(*args, "device").value_or(0);

    const bool res = view_->GetIDisplay()->SetCustomCursor(
        device, *pixels, *width, *height, hotspot_x, hotspot_y);
    result->Success(flutter::EncodableValue(res));
    return;
  }

  if (method == "clearCustomCursor") {
    int32_t device = 0;
    if (const auto* args =
            std::get_if<flutter::EncodableMap>(method_call.arguments())) {
      device = IntAt(*args, "device").value_or(0);
    }
    const bool res = view_->GetIDisplay()->ClearCustomCursor(device);
    result->Success(flutter::EncodableValue(res));
    return;
  }

  result->NotImplemented();
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
