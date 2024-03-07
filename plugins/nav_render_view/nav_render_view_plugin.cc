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

#include "nav_render_view_plugin.h"

#include <flutter/standard_message_codec.h>
#include <plugins/common/common.h>

#include "libnav_render.h"
#include "messages.h"

class FlutterView;

class Display;

namespace nav_render_view_plugin {

// static
void NavRenderViewPlugin::RegisterWithRegistrar(
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
  if (LibNavRender::IsPresent() &&
      kExpectedRenderApiVersion == LibNavRender->GetInterfaceVersion()) {
    auto plugin = std::make_unique<NavRenderViewPlugin>(
        id, std::move(viewType), direction, top, left, width, height, params,
        std::move(assetDirectory), engine, addListener, removeListener,
        platform_view_context);

    registrar->AddPlugin(std::move(plugin));
  }
}

NavRenderViewPlugin::NavRenderViewPlugin(
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
  SPDLOG_TRACE("++NavRenderViewPlugin::NavRenderViewPlugin");
  auto& codec = flutter::StandardMessageCodec::GetInstance();
  const auto decoded = codec.DecodeMessage(params.data(), params.size());
  const auto& creationParams =
      std::get_if<flutter::EncodableMap>(decoded.get());

  for (const auto& it : *creationParams) {
    if (it.second.IsNull())
      continue;
    auto key = std::get<std::string>(it.first);

    // TODO handle parameters
    plugin_common::Encodable::PrintFlutterEncodableValue(key.c_str(),
                                                         it.second);
  }

  /* Setup Wayland subsurface */
  auto flutter_view = state->view_controller->view;
  display_ = flutter_view->GetDisplay()->GetDisplay();
  egl_display_ = eglGetDisplay(display_);
  assert(egl_display_);

  surface_ =
      wl_compositor_create_surface(flutter_view->GetDisplay()->GetCompositor());
  egl_window_ = wl_egl_window_create(surface_, width_, height_);
  assert(egl_window_);

  InitializeEGL();
  egl_surface_ = eglCreateWindowSurface(
      egl_display_, egl_config_,
      reinterpret_cast<EGLNativeWindowType>(egl_window_), nullptr);

  // Subsurface
  parent_surface_ = flutter_view->GetWindow()->GetBaseSurface();
  subsurface_ = wl_subcompositor_get_subsurface(
      flutter_view->GetDisplay()->GetSubCompositor(), surface_,
      parent_surface_);

  wl_subsurface_set_desync(subsurface_);

  eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
  InitializeScene();

  addListener(platformViewsContext_, id, &platform_view_listener_, this);
  SPDLOG_TRACE("--NavRenderViewPlugin::NavRenderViewPlugin");
}

NavRenderViewPlugin::~NavRenderViewPlugin() {
  SPDLOG_TRACE("++NavRenderViewPlugin::~NavRenderViewPlugin");
  removeListener_(platformViewsContext_, id_);
  SPDLOG_TRACE("--NavRenderViewPlugin::~NavRenderViewPlugin");
}

void NavRenderViewPlugin::on_resize(double width, double height, void* data) {
  auto plugin = static_cast<NavRenderViewPlugin*>(data);
  if (plugin) {
    plugin->width_ = static_cast<int32_t>(width);
    plugin->height_ = static_cast<int32_t>(height);
    SPDLOG_TRACE("Resize: {} {}", width, height);
  }
}

void NavRenderViewPlugin::on_set_direction(int32_t direction, void* data) {
  auto plugin = static_cast<NavRenderViewPlugin*>(data);
  if (plugin) {
    plugin->direction_ = direction;
    SPDLOG_TRACE("SetDirection: {}", plugin->direction_);
  }
}

void NavRenderViewPlugin::on_set_offset(double left, double top, void* data) {
  auto plugin = static_cast<NavRenderViewPlugin*>(data);
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

void NavRenderViewPlugin::on_touch(int32_t /* action */,
                                   int32_t /* point_count */,
                                   const size_t /* point_data_size */,
                                   const double* /* point_data */,
                                   void* /* data */) {
  //auto plugin = static_cast<NavRenderViewPlugin*>(data);
}

void NavRenderViewPlugin::on_dispose(bool /* hybrid */, void* data) {
  auto plugin = static_cast<NavRenderViewPlugin*>(data);
  if (plugin->callback_) {
    wl_callback_destroy(plugin->callback_);
    plugin->callback_ = nullptr;
  }

  if (plugin->subsurface_) {
    wl_subsurface_destroy(plugin->subsurface_);
    plugin->subsurface_ = nullptr;
  }

  if (plugin->egl_window_) {
    wl_egl_window_destroy(plugin->egl_window_);
    plugin->egl_window_ = nullptr;
  }

  if (plugin->surface_) {
    wl_surface_destroy(plugin->surface_);
    plugin->surface_ = nullptr;
  }
}

const struct platform_view_listener
    NavRenderViewPlugin::platform_view_listener_ = {
        .resize = on_resize,
        .set_direction = on_set_direction,
        .set_offset = on_set_offset,
        .on_touch = on_touch,
        .dispose = on_dispose};

void NavRenderViewPlugin::on_frame(void* data,
                                   wl_callback* callback,
                                   const uint32_t time) {
  const auto obj = static_cast<NavRenderViewPlugin*>(data);

  obj->callback_ = nullptr;

  if (callback) {
    wl_callback_destroy(callback);
  }

  obj->DrawFrame(time);

  // Z-Order
  // wl_subsurface_place_above(obj->subsurface_, obj->parent_surface_);
  wl_subsurface_place_below(obj->subsurface_, obj->parent_surface_);

  obj->callback_ = wl_surface_frame(obj->surface_);
  wl_callback_add_listener(obj->callback_, &NavRenderViewPlugin::frame_listener,
                           data);

  wl_subsurface_set_position(obj->subsurface_, obj->left_, obj->top_);

  wl_surface_commit(obj->surface_);
}

const wl_callback_listener NavRenderViewPlugin::frame_listener = {.done =
                                                                      on_frame};

void NavRenderViewPlugin::InitializeEGL() {
  EGLint major, minor;
  EGLBoolean ret = eglInitialize(egl_display_, &major, &minor);
  assert(ret == EGL_TRUE);

  ret = eglBindAPI(EGL_OPENGL_ES_API);
  assert(ret == EGL_TRUE);

  EGLint count;
  eglGetConfigs(egl_display_, nullptr, 0, &count);
  assert(count);
  SPDLOG_TRACE("EGL has {} configs", count);

  auto* configs = static_cast<EGLConfig*>(
      calloc(static_cast<size_t>(count), sizeof(EGLConfig)));
  assert(configs);

  EGLint n;
  ret = eglChooseConfig(egl_display_, kEglConfigAttribs.data(), configs, count,
                        &n);
  assert(ret && n >= 1);

  EGLint size;
  for (EGLint i = 0; i < n; i++) {
    eglGetConfigAttrib(egl_display_, configs[i], EGL_BUFFER_SIZE, &size);
    SPDLOG_TRACE("Buffer size for config {} is {}", i, size);
    if (buffer_size_ <= size) {
      memcpy(&egl_config_, &configs[i], sizeof(EGLConfig));
      break;
    }
  }
  free(configs);
  if (egl_config_ == nullptr) {
    SPDLOG_CRITICAL("did not find config with buffer size {}", buffer_size_);
    assert(false);
  }

  egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT,
                                  kEglContextAttribs.data());
  assert(egl_context_);
  SPDLOG_TRACE("Context={}", egl_context_);
}

void NavRenderViewPlugin::InitializeScene() {}

void NavRenderViewPlugin::DrawFrame(uint32_t /* time */) const {
  if (eglGetCurrentContext() != egl_context_) {
    eglMakeCurrent(egl_display_, egl_surface_, egl_surface_, egl_context_);
  }

  glViewport(0, 0, width_, height_);
  glClear(GL_COLOR_BUFFER_BIT);
  glUseProgram(programObject_);

  eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

}  // namespace nav_render_view_plugin