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

#include "shell/platform/homescreen/platform_views/platform_view_listener.h"
#include "shell/platform/homescreen/platform_views/platform_view_touch.h"

#include "plugins/common/common.h"

#include "plugins/filament_view/include/filament_view/filament_view_plugin_c_api.h"
#include "plugins/layer_playground_view/include/layer_playground_view/layer_playground_view_plugin_c_api.h"
#include "plugins/nav_render_view/include/nav_render_view/nav_render_view_plugin_c_api.h"
#include "plugins/webview_flutter_view/include/webview_flutter_view/webview_flutter_view_plugin_c_api.h"

static constexpr char kMethodCreate[] = "create";
static constexpr char kMethodDispose[] = "dispose";
static constexpr char kMethodResize[] = "resize";
static constexpr char kMethodSetDirection[] = "setDirection";
static constexpr char kMethodClearFocus[] = "clearFocus";
static constexpr char kMethodOffset[] = "offset";
static constexpr char kMethodTouch[] = "touch";
static constexpr char kMethodAcceptGesture[] = "acceptGesture";
static constexpr char kMethodRejectGesture[] = "rejectGesture";

static constexpr char kKeyId[] = "id";
static constexpr char kKeyViewType[] = "viewType";
static constexpr char kKeyDirection[] = "direction";
static constexpr char kKeyWidth[] = "width";
static constexpr char kKeyHeight[] = "height";
static constexpr char kKeyParams[] = "params";
static constexpr char kKeyTop[] = "top";
static constexpr char kKeyLeft[] = "left";
static constexpr char kKeyHybrid[] = "hybrid";

static constexpr bool kPlatformViewDebug = false;

PlatformViewsHandler::PlatformViewsHandler(flutter::BinaryMessenger* messenger,
                                           FlutterDesktopEngineRef engine)
    : channel_(
          std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
              messenger,
              "flutter/platform_views",
              &flutter::StandardMethodCodec::GetInstance())),
      engine_(engine) {
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
    double top = 0;
    double left = 0;
    double width = 0;
    double height = 0;
    std::vector<uint8_t> params{};
    if (kPlatformViewDebug) {
      plugin_common::Encodable::PrintFlutterEncodableValue(kMethodCreate,
                                                           *arguments);
    }

    const auto args = std::get_if<flutter::EncodableMap>(arguments);

    for (const auto& [fst, snd] : *args) {
      if (const auto key = std::get<std::string>(fst);
          key == kKeyDirection && std::holds_alternative<int32_t>(snd)) {
        direction = std::get<int32_t>(snd);
      } else if (key == kKeyHeight && std::holds_alternative<double>(snd)) {
        height = std::get<double>(snd);
      } else if (key == kKeyId && std::holds_alternative<int32_t>(snd)) {
        id = std::get<int32_t>(snd);
      } else if (key == kKeyParams &&
                 std::holds_alternative<std::vector<uint8_t>>(snd)) {
        params = std::get<std::vector<uint8_t>>(snd);
      } else if (key == kKeyViewType &&
                 std::holds_alternative<std::string>(snd)) {
        viewType.assign(std::get<std::string>(snd));
      } else if (key == kKeyWidth && std::holds_alternative<double>(snd)) {
        width = std::get<double>(snd);
      } else if (key == kKeyTop && std::holds_alternative<double>(snd)) {
        top = std::get<double>(snd);
      } else if (key == kKeyLeft && std::holds_alternative<double>(snd)) {
        left = std::get<double>(snd);
      } else {
        plugin_common::Encodable::PrintFlutterEncodableValue(kMethodCreate,
                                                             *arguments);
      }
    }

    if (width == 0 || height == 0) {
      spdlog::critical(
          "[platform_views_handler] UiKitView is not supported.  Change to "
          "AndroidView or PlatformView");
      exit(EXIT_FAILURE);
    }

    auto registrar =
        FlutterDesktopGetPluginRegistrar(engine_, viewType.c_str());

#if ENABLE_PLUGIN_WEBVIEW_FLUTTER_VIEW
    if (viewType == "plugins.flutter.io/webview") {
      WebviewFlutterPluginCApiPlatformViewCreate(
          id, std::move(viewType), direction, top, left, width, height, params,
          engine_->flutter_asset_directory, engine_, &PlatformViewAddListener,
          &PlatformViewRemoveListener, this);
      result->Success(flutter::EncodableValue(id));
    } else
#endif
#if ENABLE_PLUGIN_FILAMENT_VIEW
        if (viewType == "io.sourcya.playx.3d.scene.channel_3d_scene") {
      FilamentViewPluginCApiRegisterWithRegistrar(
          registrar, id, std::move(viewType), direction, top, left, width,
          height, params, engine_->flutter_asset_directory, engine_,
          &PlatformViewAddListener, &PlatformViewRemoveListener, this);
      result->Success(flutter::EncodableValue(id));
    } else
#endif
#if ENABLE_PLUGIN_LAYER_PLAYGROUND_VIEW
        if (viewType == "@views/simple-box-view-type") {
      LayerPlaygroundPluginCApiRegisterWithRegistrar(
          registrar, id, std::move(viewType), direction, top, left, width,
          height, params, engine_->flutter_asset_directory, engine_,
          &PlatformViewAddListener, &PlatformViewRemoveListener, this);
      result->Success(flutter::EncodableValue(id));
    } else
#endif
#if ENABLE_PLUGIN_NAV_RENDER_VIEW
        if (viewType == "views/nav-render-view") {
      NavRenderViewPluginCApiRegisterWithRegistrar(
          registrar, id, std::move(viewType), direction, top, left, width,
          height, params, engine_->flutter_asset_directory, engine_,
          &PlatformViewAddListener, &PlatformViewRemoveListener, this);
      result->Success(flutter::EncodableValue(id));
    } else
#endif
    {
      (void)registrar;
      (void)id;
      (void)direction;
      (void)top;
      (void)left;
      (void)width;
      (void)height;
      (void)params;
      spdlog::error("Platform View type not registered: {}", viewType);
      result->NotImplemented();
    }
  } else if (method_name == kMethodDispose) {
    int32_t id = 0;
    bool hybrid{};
    if (kPlatformViewDebug) {
      plugin_common::Encodable::PrintFlutterEncodableValue(kMethodDispose,
                                                           *arguments);
    }
    if (const auto args = std::get_if<flutter::EncodableMap>(arguments);
        args != nullptr) {
      for (const auto& [fst, snd] : *args) {
        if (kKeyId == std::get<std::string>(fst) &&
            std::holds_alternative<int32_t>(snd)) {
          id = std::get<int32_t>(snd);
        } else if (kKeyHybrid == std::get<std::string>(fst) &&
                   std::holds_alternative<bool>(snd)) {
          hybrid = std::get<bool>(snd);
        } else {
          plugin_common::Encodable::PrintFlutterEncodableValue(kMethodDispose,
                                                               *arguments);
        }
      }
    }

    if (listeners_.find(id) != listeners_.end()) {
      auto [fst, snd] = listeners_[id];
      if (auto callbacks = fst; callbacks->dispose) {
        callbacks->dispose(hybrid, snd);
      }
    }

    result->Success();

  } else if (method_name == kMethodResize) {
    if (kPlatformViewDebug) {
      plugin_common::Encodable::PrintFlutterEncodableValue(kMethodResize,
                                                           *arguments);
    }

    int32_t id = 0;
    double width = 0;
    double height = 0;

    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (const auto& [fst, snd] : *args) {
      if (kKeyId == std::get<std::string>(fst) &&
          std::holds_alternative<int32_t>(snd)) {
        id = std::get<int32_t>(snd);
      } else if (kKeyWidth == std::get<std::string>(fst) &&
                 std::holds_alternative<double>(snd)) {
        width = std::get<double>(snd);
      } else if (kKeyHeight == std::get<std::string>(fst) &&
                 std::holds_alternative<double>(snd)) {
        height = std::get<double>(snd);
      } else {
        plugin_common::Encodable::PrintFlutterEncodableValue(kMethodResize,
                                                             *arguments);
      }
    }

    if (listeners_.find(id) != listeners_.end()) {
      auto [fst, snd] = listeners_[id];
      if (auto callbacks = fst; callbacks->resize) {
        callbacks->resize(width, height, snd);
      }
    }

    const auto res = flutter::EncodableValue(flutter::EncodableMap{
        {flutter::EncodableValue("id"), flutter::EncodableValue(id)},
        {flutter::EncodableValue("width"), flutter::EncodableValue(width)},
        {flutter::EncodableValue("height"), flutter::EncodableValue(height)},
    });
    result->Success(res);
  } else if (method_name == kMethodSetDirection) {
    if (kPlatformViewDebug) {
      plugin_common::Encodable::PrintFlutterEncodableValue(kMethodSetDirection,
                                                           *arguments);
    }

    int32_t id = 0;
    int32_t direction = 0;
    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (const auto& [fst, snd] : *args) {
      if (kKeyId == std::get<std::string>(fst) &&
          std::holds_alternative<int32_t>(snd)) {
        id = std::get<int32_t>(snd);
      } else if (kKeyDirection == std::get<std::string>(fst) &&
                 std::holds_alternative<int32_t>(snd)) {
        direction = std::get<int32_t>(snd);
      } else {
        plugin_common::Encodable::PrintFlutterEncodableValue(
            kMethodSetDirection, *arguments);
      }
    }
    if (listeners_.find(id) != listeners_.end()) {
      auto [fst, snd] = listeners_[id];
      if (auto callbacks = fst; callbacks->set_direction) {
        callbacks->set_direction(direction, snd);
      }
    }
    result->Success();
  } else if (method_name == kMethodClearFocus) {
    if (kPlatformViewDebug) {
      plugin_common::Encodable::PrintFlutterEncodableValue(kMethodClearFocus,
                                                           *arguments);
    }
    result->Success();
  } else if (method_name == kMethodOffset) {
    if (kPlatformViewDebug) {
      plugin_common::Encodable::PrintFlutterEncodableValue(kMethodOffset,
                                                           *arguments);
    }
    int32_t id = 0;
    double left = 0;
    double top = 0;
    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (const auto& [fst, snd] : *args) {
      if (kKeyId == std::get<std::string>(fst) &&
          std::holds_alternative<int32_t>(snd)) {
        id = std::get<int32_t>(snd);
      } else if (kKeyLeft == std::get<std::string>(fst) &&
                 std::holds_alternative<double>(snd)) {
        left = std::get<double>(snd);
      } else if (kKeyTop == std::get<std::string>(fst) &&
                 std::holds_alternative<double>(snd)) {
        top = std::get<double>(snd);
      } else {
        plugin_common::Encodable::PrintFlutterEncodableValue(kMethodOffset,
                                                             *arguments);
      }
    }
    if (listeners_.find(id) != listeners_.end()) {
      auto [fst, snd] = listeners_[id];
      if (auto callbacks = fst; callbacks->set_offset) {
        callbacks->set_offset(left, top, snd);
      }
    }
    result->Success();
  } else if (method_name == kMethodTouch && !arguments->IsNull()) {
    if (kPlatformViewDebug) {
      plugin_common::Encodable::PrintFlutterEncodableValue(kMethodTouch,
                                                           *arguments);
    }

    /// The user touched a platform view within Flutter.
    const auto& params = std::get_if<flutter::EncodableList>(arguments);
    auto touch = PlatformViewTouch(*params);
    SPDLOG_TRACE("PlatformViewTouch id: {}", touch.getId());
    auto id = touch.getId();
    if (listeners_.find(id) != listeners_.end()) {
      auto [fst, snd] = listeners_[id];
      if (auto callbacks = fst; callbacks->on_touch) {
        callbacks->on_touch(touch.getAction(), touch.getPointerCount(),
                            touch.getRawPointerCoords().size(),
                            touch.getRawPointerCoords().data(), snd);
      }
    }
    result->Success();
  } else if (method_name == kMethodAcceptGesture && !arguments->IsNull()) {
    if (kPlatformViewDebug) {
      plugin_common::Encodable::PrintFlutterEncodableValue(kMethodAcceptGesture,
                                                           *arguments);
    }

    int32_t id = 0;
    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (const auto& [fst, snd] : *args) {
      if (kKeyId == std::get<std::string>(fst) &&
          std::holds_alternative<int32_t>(snd)) {
        id = std::get<int32_t>(snd);
      } else {
        plugin_common::Encodable::PrintFlutterEncodableValue(
            kMethodAcceptGesture, *arguments);
      }
    }
    if (listeners_.find(id) != listeners_.end()) {
      auto [fst, snd] = listeners_[id];
      if (auto callbacks = fst; callbacks->accept_gesture) {
        callbacks->accept_gesture(id);
      }
    }
    result->Success();
  } else if (method_name == kMethodRejectGesture && !arguments->IsNull()) {
    if (kPlatformViewDebug) {
      plugin_common::Encodable::PrintFlutterEncodableValue(kMethodRejectGesture,
                                                           *arguments);
    }
    int32_t id = 0;
    const auto args = std::get_if<flutter::EncodableMap>(arguments);
    for (const auto& [fst, snd] : *args) {
      if (kKeyId == std::get<std::string>(fst) &&
          std::holds_alternative<int32_t>(snd)) {
        id = std::get<int32_t>(snd);
      } else {
        plugin_common::Encodable::PrintFlutterEncodableValue(
            kMethodRejectGesture, *arguments);
      }
    }
    if (listeners_.find(id) != listeners_.end()) {
      auto [fst, snd] = listeners_[id];
      if (auto callbacks = fst; callbacks->reject_gesture) {
        callbacks->reject_gesture(id);
      }
    }
    result->Success();
  } else {
    spdlog::error("[PlatformViews] method {} is unhandled", method_name);
    plugin_common::Encodable::PrintFlutterEncodableValue(method_name.c_str(),
                                                         *arguments);
    result->NotImplemented();
  }
}

void PlatformViewsHandler::PlatformViewAddListener(
    void* context,
    const int32_t id,
    const platform_view_listener* listener,
    void* listener_context) {
  if (const auto platformView = static_cast<PlatformViewsHandler*>(context);
      platformView->listeners_.find(id) != platformView->listeners_.end()) {
    platformView->listeners_.erase(id);
  } else {
    platformView->listeners_[id] = std::make_pair(listener, listener_context);
  }
}

void PlatformViewsHandler::PlatformViewRemoveListener(void* context,
                                                      const int32_t id) {
  if (auto platformView = static_cast<PlatformViewsHandler*>(context);
      platformView->listeners_.find(id) != platformView->listeners_.end()) {
    platformView->listeners_.erase(id);
  }
}
