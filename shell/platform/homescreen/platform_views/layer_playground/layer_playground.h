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

#pragma once

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <wayland-client.h>

#include "platform_views/platform_view.h"
#include "shell/view/flutter_view.h"

class LayerPlayground final : public PlatformView {
 public:
  LayerPlayground(int32_t id,
                  std::string viewType,
                  int32_t direction,
                  double width,
                  double height,
                  FlutterView* view);
  ~LayerPlayground() override;

  void Resize(double width, double height) override;

  void SetDirection(int32_t direction) override;

  void SetOffset(double left, double top) override;

  void Dispose(bool hybrid) override;

 private:
  wl_display* display_;
  wl_surface* surface_;
  wl_surface* parent_surface_;
  wl_callback* callback_;
  wl_subsurface* subsurface_;

  static void on_frame(void* data, wl_callback* callback, uint32_t time);
  static const wl_callback_listener frame_listener;

  EGLDisplay egl_display_;
  struct wl_egl_window* egl_window_;
  int buffer_size_ = 32;
  EGLContext egl_context_{};
  EGLConfig egl_config_{};
  GLuint programObject_{};
  EGLSurface egl_surface_{};

  void InitializeEGL();
  void InitializeScene();
  void DrawFrame(uint32_t time) const;

};