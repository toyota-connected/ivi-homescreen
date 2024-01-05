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

#include "webview_flutter_plugin.h"

#include "messages.g.h"

#include <flutter/plugin_registrar.h>

#include <memory>

#include "logging/logging.h"

namespace plugin_webview_flutter {

// static
void WebviewFlutterPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrar* registrar,
    int32_t id,
    std::string viewType,
    int32_t direction,
    double top,
    double left,
    double width,
    double height,
    const std::vector<uint8_t>& params,
    std::string assetDirectory,
    FlutterDesktopEngineRef engine,
    PlatformViewAddListener addListener,
    PlatformViewRemoveListener removeListener,
    void* platform_view_context) {
  auto plugin = std::make_unique<WebviewFlutterPlugin>(
      id, std::move(viewType), direction, top, left, width, height, params,
      std::move(assetDirectory), engine, addListener, removeListener,
      platform_view_context);

  InstanceManagerHostApi::SetUp(registrar->messenger(), plugin.get());
  WebStorageHostApi::SetUp(registrar->messenger(), plugin.get());
  WebViewHostApi::SetUp(registrar->messenger(), plugin.get());
  WebSettingsHostApi::SetUp(registrar->messenger(), plugin.get());
  WebChromeClientHostApi::SetUp(registrar->messenger(), plugin.get());
  WebViewClientHostApi::SetUp(registrar->messenger(), plugin.get());
  DownloadListenerHostApi::SetUp(registrar->messenger(), plugin.get());
  JavaScriptChannelHostApi::SetUp(registrar->messenger(), plugin.get());

  registrar->AddPlugin(std::move(plugin));
}

WebviewFlutterPlugin::WebviewFlutterPlugin(
    int32_t id,
    std::string viewType,
    int32_t direction,
    double top,
    double left,
    double width,
    double height,
    const std::vector<uint8_t>& params,
    std::string assetDirectory,
    FlutterDesktopEngineState* state,
    PlatformViewAddListener addListener,
    PlatformViewRemoveListener removeListener,
    void* platform_view_context)
    : PlatformView(id,
                   std::move(viewType),
                   direction,
                   top,
                   left,
                   width,
                   height),
      id_(id),
      platformViewsContext_(platform_view_context),
      removeListener_(removeListener),
      flutterAssetsPath_(std::move(assetDirectory)),
      callback_(nullptr) {
  SPDLOG_TRACE("++WebviewFlutterPlugin::WebviewFlutterPlugin");

  /* Setup Wayland subsurface */
  auto flutter_view = state->view_controller->view;
  display_ = flutter_view->GetDisplay()->GetDisplay();
  parent_surface_ = flutter_view->GetWindow()->GetBaseSurface();
  surface_ =
      wl_compositor_create_surface(flutter_view->GetDisplay()->GetCompositor());
  subsurface_ = wl_subcompositor_get_subsurface(
      flutter_view->GetDisplay()->GetSubCompositor(), surface_,
      parent_surface_);

  // wl_subsurface_set_sync(subsurface_);
  wl_subsurface_set_desync(subsurface_);
  wl_subsurface_set_position(subsurface_, static_cast<int32_t>(top),
                             static_cast<int32_t>(left));
  wl_subsurface_place_above(subsurface_, parent_surface_);
  //wl_subsurface_place_below(subsurface_, surface_);
  wl_surface_commit(parent_surface_);

  addListener(platformViewsContext_, id, &platform_view_listener_, this);
  SPDLOG_TRACE("--WebviewFlutterPlugin::WebviewFlutterPlugin");
}

WebviewFlutterPlugin::~WebviewFlutterPlugin() = default;

std::optional<FlutterError> WebviewFlutterPlugin::Clear() {
  SPDLOG_DEBUG("[webview_flutter] Clear");
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::Create(int64_t instance_id) {
  SPDLOG_DEBUG("[webview_flutter] Create {}", instance_id);
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::DeleteAllData(
    int64_t instance_id) {
  SPDLOG_DEBUG("[webview_flutter] DeleteAllData: {}", instance_id);
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::LoadData(
    int64_t instance_id,
    const std::string& data,
    const std::string* mime_type,
    const std::string* encoding) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::LoadDataWithBaseUrl(
    int64_t instance_id,
    const std::string* base_url,
    const std::string& data,
    const std::string* mime_type,
    const std::string* encoding,
    const std::string* history_url) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::LoadUrl(
    int64_t instance_id,
    const std::string& url,
    const flutter::EncodableMap& headers) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::PostUrl(
    int64_t instance_id,
    const std::string& url,
    const std::vector<uint8_t>& data) {
  return std::nullopt;
}

ErrorOr<std::optional<std::string>> WebviewFlutterPlugin::GetUrl(
    int64_t instance_id) {
  return {std::nullopt};
}

ErrorOr<bool> WebviewFlutterPlugin::CanGoBack(int64_t instance_id) {
  return {true};
}

ErrorOr<bool> WebviewFlutterPlugin::CanGoForward(int64_t instance_id) {
  return {true};
}

std::optional<FlutterError> WebviewFlutterPlugin::GoBack(int64_t instance_id) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::GoForward(
    int64_t instance_id) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::Reload(int64_t instance_id) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::ClearCache(
    int64_t instance_id,
    bool include_disk_files) {
  return std::nullopt;
}

void WebviewFlutterPlugin::EvaluateJavascript(
    int64_t instance_id,
    const std::string& javascript_string,
    std::function<void(ErrorOr<std::optional<std::string>> reply)> result) {}

std::optional<FlutterError> WebviewFlutterPlugin::Create(
    int64_t instance_id,
    const std::string& channel_name) {
  return std::nullopt;
}

ErrorOr<std::optional<std::string>> WebviewFlutterPlugin::GetTitle(
    int64_t instance_id) {
  return {std::nullopt};
}
std::optional<FlutterError> WebviewFlutterPlugin::ScrollTo(int64_t instance_id,
                                                           int64_t x,
                                                           int64_t y) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::ScrollBy(int64_t instance_id,
                                                           int64_t x,
                                                           int64_t y) {
  return std::nullopt;
}

ErrorOr<int64_t> WebviewFlutterPlugin::GetScrollX(int64_t instance_id) {
  return {0};
}

ErrorOr<int64_t> WebviewFlutterPlugin::GetScrollY(int64_t instance_id) {
  return {0};
}

ErrorOr<WebViewPoint> WebviewFlutterPlugin::GetScrollPosition(
    int64_t instance_id) {
  return {WebViewPoint{0, 0}};
}

std::optional<FlutterError>
WebviewFlutterPlugin::SetWebContentsDebuggingEnabled(bool enabled) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetWebViewClient(
    int64_t instance_id,
    int64_t web_view_client_instance_id) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::AddJavaScriptChannel(
    int64_t instance_id,
    int64_t java_script_channel_instance_id) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::RemoveJavaScriptChannel(
    int64_t instance_id,
    int64_t java_script_channel_instance_id) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetDownloadListener(
    int64_t instance_id,
    const int64_t* listener_instance_id) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetWebChromeClient(
    int64_t instance_id,
    const int64_t* client_instance_id) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetBackgroundColor(
    int64_t instance_id,
    int64_t color) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::Create(
    int64_t instance_id,
    int64_t web_view_instance_id) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetDomStorageEnabled(
    int64_t instance_id,
    bool flag) {
  return std::nullopt;
}

std::optional<FlutterError>
WebviewFlutterPlugin::SetJavaScriptCanOpenWindowsAutomatically(
    int64_t instance_id,
    bool flag) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetSupportMultipleWindows(
    int64_t instance_id,
    bool support) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetJavaScriptEnabled(
    int64_t instance_id,
    bool flag) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetUserAgentString(
    int64_t instance_id,
    const std::string* user_agent_string) {
  return std::nullopt;
}

std::optional<FlutterError>
WebviewFlutterPlugin::SetMediaPlaybackRequiresUserGesture(int64_t instance_id,
                                                          bool require) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetSupportZoom(
    int64_t instance_id,
    bool support) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetLoadWithOverviewMode(
    int64_t instance_id,
    bool overview) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetUseWideViewPort(
    int64_t instance_id,
    bool use) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetDisplayZoomControls(
    int64_t instance_id,
    bool enabled) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetBuiltInZoomControls(
    int64_t instance_id,
    bool enabled) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetAllowFileAccess(
    int64_t instance_id,
    bool enabled) {
  return std::nullopt;
}

std::optional<FlutterError> WebviewFlutterPlugin::SetTextZoom(
    int64_t instance_id,
    int64_t text_zoom) {
  return std::nullopt;
}

ErrorOr<std::string> WebviewFlutterPlugin::GetUserAgentString(
    int64_t instance_id) {
  return {""};
}

std::optional<FlutterError>
WebviewFlutterPlugin::SetSynchronousReturnValueForOnShowFileChooser(
    int64_t instance_id,
    bool value) {
  return std::nullopt;
}

std::optional<FlutterError>
WebviewFlutterPlugin::SetSynchronousReturnValueForOnConsoleMessage(
    int64_t instance_id,
    bool value) {
  return std::nullopt;
}

std::optional<FlutterError>
WebviewFlutterPlugin::SetSynchronousReturnValueForShouldOverrideUrlLoading(
    int64_t instance_id,
    bool value) {
  return std::nullopt;
}

void WebviewFlutterPlugin::on_resize(double width, double height, void* data) {
  auto plugin = static_cast<WebviewFlutterPlugin*>(data);
  if (plugin) {
    plugin->width_ = static_cast<int32_t>(width);
    plugin->height_ = static_cast<int32_t>(height);
    SPDLOG_TRACE("Resize: {} {}", width, height);
  }
}

void WebviewFlutterPlugin::on_set_direction(int32_t direction, void* data) {
  auto plugin = static_cast<WebviewFlutterPlugin*>(data);
  if (plugin) {
    plugin->direction_ = direction;
    SPDLOG_TRACE("SetDirection: {}", plugin->direction_);
  }
}

void WebviewFlutterPlugin::on_set_offset(double left, double top, void* data) {
  auto plugin = static_cast<WebviewFlutterPlugin*>(data);
  if (plugin) {
    plugin->left_ = static_cast<int32_t>(left);
    plugin->top_ = static_cast<int32_t>(top);
    if (plugin->subsurface_) {
      SPDLOG_DEBUG("SetOffset: left: {}, top: {}", plugin->left_, plugin->top_);
      wl_subsurface_set_position(plugin->subsurface_, plugin->left_,
                                 plugin->top_);
      if (!plugin->callback_) {
        on_frame(plugin, plugin->callback_, 0);
      }
    }
  }
}

void WebviewFlutterPlugin::on_touch(int32_t action,
                                    double x,
                                    double y,
                                    void* data) {
  auto plugin = static_cast<WebviewFlutterPlugin*>(data);
}

void WebviewFlutterPlugin::on_dispose(bool hybrid, void* data) {
  auto plugin = static_cast<WebviewFlutterPlugin*>(data);
  if (plugin->callback_) {
    wl_callback_destroy(plugin->callback_);
    plugin->callback_ = nullptr;
  }

  if (plugin->subsurface_) {
    wl_subsurface_destroy(plugin->subsurface_);
    plugin->subsurface_ = nullptr;
  }

  if (plugin->surface_) {
    wl_surface_destroy(plugin->surface_);
    plugin->surface_ = nullptr;
  }
}

const struct platform_view_listener
    WebviewFlutterPlugin::platform_view_listener_ = {
        .resize = on_resize,
        .set_direction = on_set_direction,
        .set_offset = on_set_offset,
        .on_touch = on_touch,
        .dispose = on_dispose};

void WebviewFlutterPlugin::on_frame(void* data,
                                    wl_callback* callback,
                                    const uint32_t time) {
  const auto obj = static_cast<WebviewFlutterPlugin*>(data);

  obj->callback_ = nullptr;

  if (callback) {
    wl_callback_destroy(callback);
  }

  // TODO obj->DrawFrame(time);

  // Z-Order
  // wl_subsurface_place_above(obj->subsurface_, obj->parent_surface_);
  wl_subsurface_place_below(obj->subsurface_, obj->parent_surface_);

  obj->callback_ = wl_surface_frame(obj->surface_);
  wl_callback_add_listener(obj->callback_,
                           &WebviewFlutterPlugin::frame_listener, data);

  wl_subsurface_set_position(obj->subsurface_, obj->left_, obj->top_);

  wl_surface_commit(obj->surface_);
}

const wl_callback_listener WebviewFlutterPlugin::frame_listener = {
    .done = on_frame};

}  // namespace plugin_webview_flutter
