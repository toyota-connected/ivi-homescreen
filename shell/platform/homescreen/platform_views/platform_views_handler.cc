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
#include "logging/logging.h"

#include <generated_plugin_registrant.h>

#include <tools/encodable.h>

#include "config/plugins.h"
#include "platform_view_touch.h"

#if BUILD_COMPOSITOR
#include "flutter_desktop_engine_state.h"
#include "flutter_desktop_view_controller_state.h"
#include "view/flutter_view.h"
#endif

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
static constexpr char kKeyDirection[] = "direction";
static constexpr char kKeyWidth[] = "width";
static constexpr char kKeyHeight[] = "height";
static constexpr char kKeyTop[] = "top";
static constexpr char kKeyLeft[] = "left";
static constexpr char kKeyHybrid[] = "hybrid";

static constexpr bool kPlatformViewDebug = false;

namespace {
// A platform-view method argument is either a bare integer view id (UiKitView
// sends its dispose id this way) or a map carrying an "id" key (the
// AndroidView-style create/resize/gesture messages). Resolve the id from either
// shape and from either integer width; returns false when none is present.
// Without this, a bare-int dispose fell through to id 0, so no view was ever
// unregistered and the compositor-surface map grew without bound.
bool ResolveViewId(const flutter::EncodableValue* arguments, int32_t& out_id) {
  if (arguments == nullptr) {
    return false;
  }
  if (const auto* v = std::get_if<int32_t>(arguments)) {
    out_id = *v;
    return true;
  }
  if (const auto* v = std::get_if<int64_t>(arguments)) {
    out_id = static_cast<int32_t>(*v);
    return true;
  }
  if (const auto* map = std::get_if<flutter::EncodableMap>(arguments)) {
    for (const auto& [key, value] : *map) {
      const auto* key_str = std::get_if<std::string>(&key);
      if (key_str == nullptr || *key_str != kKeyId) {
        continue;
      }
      if (const auto* i32 = std::get_if<int32_t>(&value)) {
        out_id = *i32;
        return true;
      }
      if (const auto* i64 = std::get_if<int64_t>(&value)) {
        out_id = static_cast<int32_t>(*i64);
        return true;
      }
    }
  }
  return false;
}
}  // namespace

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
    PluginsAoiPlatformViewCreate(engine_, engine_->flutter_asset_directory,
                                 arguments, &PlatformViewAddListener,
                                 &PlatformViewRemoveListener, this,
                                 std::move(result));
  } else if (method_name == kMethodDispose) {
    int32_t id = 0;
    bool hybrid{};
    if (kPlatformViewDebug) {
      plugin_common::Encodable::PrintFlutterEncodableValue(kMethodDispose,
                                                           *arguments);
    }
    // UiKitView disposes with a bare integer id; AndroidView with a map.
    // Resolve either — an unresolved id would default to 0 and leak every real
    // view.
    if (!ResolveViewId(arguments, id)) {
      ihs::log::warn("[PlatformViews] dispose with no resolvable id; ignoring");
      result->Success();
      return;
    }
    if (const auto args = std::get_if<flutter::EncodableMap>(arguments);
        args != nullptr) {
      for (const auto& [fst, snd] : *args) {
        if (kKeyHybrid == std::get<std::string>(fst) &&
            std::holds_alternative<bool>(snd)) {
          hybrid = std::get<bool>(snd);
        }
      }
    }

    if (listeners_.find(id) != listeners_.end()) {
      auto [fst, snd] = listeners_[id];
      if (auto callbacks = fst; callbacks->dispose) {
        callbacks->dispose(hybrid, snd);
      }
    }

#if BUILD_COMPOSITOR
    // Safety net: ensure the compositor surface is unregistered even if the
    // plugin forgot. The view may be null in headless contexts.
    if (engine_ && engine_->view_controller && engine_->view_controller->view) {
      engine_->view_controller->view->UnregisterCompositorSurface(id);
    }
#endif

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

#if BUILD_COMPOSITOR
    if (engine_ && engine_->view_controller && engine_->view_controller->view) {
      engine_->view_controller->view->ResizeCompositorSurface(
          id, static_cast<int32_t>(width), static_cast<int32_t>(height));
    }
#endif

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
    const auto touch = PlatformViewTouch(*params);
    IHS_TRACE("PlatformViewTouch id: {}", touch.getId());
    if (const auto id = touch.getId();
        listeners_.find(id) != listeners_.end()) {
      auto [fst, snd] = listeners_[id];
      if (const auto callbacks = fst; callbacks->on_touch) {
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
      if (const auto callbacks = fst; callbacks->accept_gesture) {
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
    ihs::log::error("[PlatformViews] method {} is unhandled", method_name);
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
  if (const auto platformView = static_cast<PlatformViewsHandler*>(context);
      platformView->listeners_.find(id) != platformView->listeners_.end()) {
    platformView->listeners_.erase(id);
  }
}
