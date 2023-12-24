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

#if defined(ENABLE_PLUGIN_FILAMENT_VIEW)
#include "plugins/filament_view/include/filament_view/filament_view_plugin_c_api.h"
#endif
#if defined(ENABLE_PLUGIN_LAYER_PLAYGROUND_VIEW)
#include "plugins/layer_playground_view/include/layer_playground_view/layer_playground_view_plugin_c_api.h"
#endif

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
    auto registrar =
        FlutterDesktopGetPluginRegistrar(engine_, viewType.c_str());

#if defined(ENABLE_PLUGIN_LAYER_PLAYGROUND_VIEW)
    if (viewType == "@views/simple-box-view-type") {
      LayerPlaygroundPluginCApiRegisterWithRegistrar(
        registrar, id, std::move(viewType), direction, width, height, params,
        engine_->view_controller->engine->GetAssetDirectory(), engine_,
        &PlatformViewAddListener,
        &PlatformViewRemoveListener,
        this) ;
      result->Success(flutter::EncodableValue(id));
    } else
#endif
#if defined(ENABLE_PLUGIN_FILAMENT_VIEW)
    if (viewType == "io.sourcya.playx.3d.scene.channel_3d_scene") {
      FilamentViewPluginCApiRegisterWithRegistrar(
          registrar, id, std::move(viewType), direction, width, height, params,
          engine_->view_controller->engine->GetAssetDirectory(), engine_,
          &PlatformViewAddListener,
          &PlatformViewRemoveListener,
          this) ;
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

    if(listeners_.find(id) != listeners_.end()) {
      auto delegate = listeners_[id];
      auto callbacks = delegate.first;
      callbacks->dispose(hybrid, delegate.second);
    }

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

    if(listeners_.find(id) != listeners_.end()) {
      auto delegate = listeners_[id];
      auto callbacks = delegate.first;
      callbacks->resize(width, height, delegate.second);
    }

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
    if(listeners_.find(id) != listeners_.end()) {
      auto delegate = listeners_[id];
      auto callbacks = delegate.first;
      callbacks->set_direction(direction, delegate.second);
    }
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
    if(listeners_.find(id) != listeners_.end()) {
      auto delegate = listeners_[id];
      auto callbacks = delegate.first;
      callbacks->set_offset(left, top, delegate.second);
    }
    result->Success();
  } else if (method_name == kMethodTouch && !arguments->IsNull()) {
    /// The user touched a platform view within Flutter.
    const auto& params = std::get_if<flutter::EncodableList>(arguments);
    auto touch = PlatformViewTouch(*params);
    SPDLOG_TRACE("PlatformViewTouch id: {}", touch.getId());
    auto id = touch.getId();
    if(listeners_.find(id) != listeners_.end()) {
      auto delegate = listeners_[id];
      auto callbacks = delegate.first;
      callbacks->on_touch(touch.getAction(), touch.getX(), touch.getY(), delegate.second);
    }
    result->Success();
  } else {
    spdlog::error("[PlatformViews] method {} is unhandled", method_name);
    Utils::PrintFlutterEncodableValue("unhandled", *arguments);
    result->NotImplemented();
  }
}

void PlatformViewsHandler::PlatformViewAddListener(
    void* context,
    int32_t id,
    const struct platform_view_listener* listener,
    void* listener_context) {
  auto platformView = static_cast<PlatformViewsHandler*>(context);
  if(platformView->listeners_.find(id) != platformView->listeners_.end()) {
    platformView->listeners_.erase(id);
  } else {
    platformView->listeners_[id] = std::make_pair(listener, listener_context);
  }
}

void PlatformViewsHandler::PlatformViewRemoveListener(void* context, int32_t id) {
  auto platformView = static_cast<PlatformViewsHandler*>(context);
  if(platformView->listeners_.find(id) != platformView->listeners_.end()) {
    platformView->listeners_.erase(id);
  }
}
