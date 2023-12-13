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

#include "platform_views_handler.h"
#include "platform_view_touch.h"
#if defined(ENABLE_VIEW_FILAMENT_VIEW)
#include "platform_views/filament_view/filament_view.h"
#endif
#if defined(ENABLE_VIEW_LAYER_PLAYGROUND)
#include "platform_views/layer_playground/layer_playground.h"
#endif
#include "platform_views_registry.h"

#include "shell/logging/logging.h"
#include "shell/utils.h"

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

PlatformViewsHandler::PlatformViewsHandler(flutter::BinaryMessenger* messenger,
                                           FlutterView* view)
    : channel_(
          std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
              messenger,
              "flutter/platform_views",
              &flutter::StandardMethodCodec::GetInstance())),
      view_(view) {
  channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<flutter::EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                 result) { HandleMethodCall(call, std::move(result)); });
}

void PlatformViewsHandler::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  const std::string& method_name = method_call.method_name();
  const auto arguments = method_call.arguments();

  if (arguments->IsNull()) {
    result->Error("invalid_args", "Arguments are Null");
    return;
  }

  if (method_name == kMethodCreate) {
    int32_t id = 0;
    std::string viewType;
    int32_t direction = 0;
    double width = 0;
    double height = 0;
    std::vector<uint8_t> params{};

    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (auto& it : *args) {
      const auto key = std::get<std::string>(it.first);

      if (key == kKeyDirection && std::holds_alternative<int32_t>(it.second)) {
        direction = std::get<int32_t>(it.second);
      } else if (key == kKeyHeight &&
                 std::holds_alternative<double>(it.second)) {
        height = std::get<double>(it.second);
      } else if (key == kKeyId && std::holds_alternative<int32_t>(it.second)) {
        id = std::get<int32_t>(it.second);
      } else if (key == kKeyParams &&
                 std::holds_alternative<std::vector<uint8_t>>(it.second)) {
        params = std::get<std::vector<uint8_t>>(it.second);
      } else if (key == kKeyViewType &&
                 std::holds_alternative<std::string>(it.second)) {
        viewType.assign(std::get<std::string>(it.second));
      } else if (key == kKeyWidth &&
                 std::holds_alternative<double>(it.second)) {
        width = std::get<double>(it.second);
      }
    }

#if defined(ENABLE_VIEW_LAYER_PLAYGROUND)
    if (viewType == "@views/simple-box-view-type") {
      auto platform_view = std::make_unique<LayerPlayground>(
          id, std::move(viewType), direction, width, height, view_);
      PlatformViewsRegistry::GetInstance().AddPlatformView(
          id, std::move(platform_view));
      result->Success(flutter::EncodableValue(id));
    } else
#endif
#if defined(ENABLE_VIEW_FILAMENT_VIEW)
        if (viewType == "io.sourcya.playx.3d.scene.channel_3d_scene") {
      auto platform_view = std::make_unique<view_filament_view::FilamentView>(
          id, std::move(viewType), direction, width, height, params, "", view_);
      PlatformViewsRegistry::GetInstance().AddPlatformView(
          id, std::move(platform_view));
      result->Success(flutter::EncodableValue(id));
    } else
#endif
    {
      (void)id;
      (void)direction;
      (void)width;
      (void)height;
      (void)params;
      result->NotImplemented();
    }
  } else if (method_name == kMethodDispose) {
    int32_t id = 0;
    bool hybrid{};
    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (auto& it : *args) {
      if (kKeyId == std::get<std::string>(it.first) &&
          std::holds_alternative<int32_t>(it.second)) {
        id = std::get<int32_t>(it.second);
      } else if (kKeyHybrid == std::get<std::string>(it.first) &&
                 std::holds_alternative<bool>(it.second)) {
        hybrid = std::get<bool>(it.second);
      }
    }

    PlatformViewsRegistry::GetInstance().RemovePlatformView(id, hybrid);
    result->Success();

  } else if (method_name == kMethodResize) {
    int32_t id = 0;
    double width = 0;
    double height = 0;

    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (auto& it : *args) {
      if (kKeyId == std::get<std::string>(it.first) &&
          std::holds_alternative<int32_t>(it.second)) {
        id = std::get<int32_t>(it.second);
      } else if (kKeyWidth == std::get<std::string>(it.first) &&
                 std::holds_alternative<double>(it.second)) {
        width = std::get<double>(it.second);
      } else if (kKeyHeight == std::get<std::string>(it.first) &&
                 std::holds_alternative<double>(it.second)) {
        height = std::get<double>(it.second);
      }
    }

    PlatformViewsRegistry::GetInstance().GetPlatformView(id)->Resize(width,
                                                                     height);

    const auto res = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue("id"), flutter::EncodableValue(id)},
        {flutter::EncodableValue("width"), flutter::EncodableValue(width)},
        {flutter::EncodableValue("height"), flutter::EncodableValue(height)},
    });
    result->Success(res);
  } else if (method_name == kMethodSetDirection) {
    int32_t id = 0;
    int32_t direction = 0;
    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (auto& it : *args) {
      if (kKeyId == std::get<std::string>(it.first) &&
          std::holds_alternative<int32_t>(it.second)) {
        id = std::get<int32_t>(it.second);
      } else if (kKeyDirection == std::get<std::string>(it.first) &&
                 std::holds_alternative<int32_t>(it.second)) {
        direction = std::get<int32_t>(it.second);
      }
    }
    PlatformViewsRegistry::GetInstance().GetPlatformView(id)->SetDirection(
        direction);
    result->Success();
  } else if (method_name == kMethodClearFocus) {
    Utils::PrintFlutterEncodableValue("clearFocus", *arguments);
    result->Success();
  } else if (method_name == kMethodOffset) {
    int32_t id = 0;
    double left = 0;
    double top = 0;
    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (auto& it : *args) {
      if (kKeyId == std::get<std::string>(it.first) &&
          std::holds_alternative<int32_t>(it.second)) {
        id = std::get<int32_t>(it.second);
      } else if (kKeyLeft == std::get<std::string>(it.first) &&
                 std::holds_alternative<double>(it.second)) {
        left = std::get<double>(it.second);
      } else if (kKeyTop == std::get<std::string>(it.first) &&
                 std::holds_alternative<double>(it.second)) {
        top = std::get<double>(it.second);
      }
    }
    PlatformViewsRegistry::GetInstance().GetPlatformView(id)->SetOffset(left,
                                                                        top);
    result->Success();
  } else if (method_name == kMethodTouch && !arguments->IsNull()) {
    /// The user touched a platform view within Flutter.
    const auto& params = std::get_if<flutter::EncodableList>(arguments);
    auto touch = PlatformViewTouch(*params);
    touch.Print();
    result->Success();
  } else {
    spdlog::error("[PlatformViews] method {} is unhandled", method_name);
    Utils::PrintFlutterEncodableValue("unhandled", *arguments);
    result->NotImplemented();
  }
}
