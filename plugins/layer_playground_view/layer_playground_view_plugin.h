/*
 * Copyright 2020-2023 Toyota Connected North America
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

#ifndef FLUTTER_PLUGIN_LAYER_PLAYGROUND_PLUGIN_H_
#define FLUTTER_PLUGIN_LAYER_PLAYGROUND_PLUGIN_H_

#include <memory>

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <flutter/event_channel.h>
#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "flutter_desktop_engine_state.h"
#include "flutter_homescreen.h"
#include "platform_views/platform_view.h"
#include "view/flutter_view.h"
#include "wayland/display.h"

namespace plugin_layer_playground_view {

class LayerPlaygroundViewPlugin : public flutter::Plugin, PlatformView {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar,
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
                                    void* platform_view_context);

  LayerPlaygroundViewPlugin(int32_t id,
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
                            void* platform_view_context);

  ~LayerPlaygroundViewPlugin() override;

  // Disallow copy and assign.
  LayerPlaygroundViewPlugin(const LayerPlaygroundViewPlugin&) = delete;

  LayerPlaygroundViewPlugin& operator=(const LayerPlaygroundViewPlugin&) =
      delete;

 private:
  int32_t id_;
  void* platformViewsContext_;
  PlatformViewRemoveListener removeListener_;
  const std::string flutterAssetsPath_;

  wl_display* display_;
  wl_surface* surface_;
  wl_surface* parent_surface_;
  wl_callback* callback_;
  wl_subsurface* subsurface_;

  static void on_frame(void* data, wl_callback* callback, uint32_t time);
  static const wl_callback_listener frame_listener;

  EGLDisplay egl_display_;
  wl_egl_window* egl_window_;
  int buffer_size_ = 32;
  EGLContext egl_context_{};
  EGLConfig egl_config_{};
  GLuint programObject_{};
  EGLSurface egl_surface_{};

  void InitializeEGL();
  void InitializeScene();
  void DrawFrame(uint32_t time) const;

  static void on_resize(double width, double height, void* data);
  static void on_set_direction(int32_t direction, void* data);
  static void on_set_offset(double left, double top, void* data);
  static void on_touch(int32_t action,
                       int32_t point_count,
                       const size_t point_data_size,
                       const double* point_data,
                       void* data);
  static void on_dispose(bool hybrid, void* data);

  static const struct platform_view_listener platform_view_listener_;
};

}  // namespace plugin_layer_playground_view

#endif  // FLUTTER_PLUGIN_LAYER_PLAYGROUND_PLUGIN_H_
