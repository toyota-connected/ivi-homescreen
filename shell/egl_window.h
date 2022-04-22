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

#pragma once

#include <memory>
#include <string>

#include <cassert>

#include "agl-shell-client-protocol.h"
#include "constants.h"

#include "egl.h"

class Display;

class EglWindow : public Egl {
 public:
  enum window_type { WINDOW_NORMAL, WINDOW_BG, WINDOW_TOP, WINDOW_BOTTOM };

  EglWindow(size_t index,
            const std::shared_ptr<Display>& display,
            enum window_type type,
            std::string app_id,
            bool fullscreen,
            bool debug_egl,
            int32_t width,
            int32_t height);
  ~EglWindow();
  EglWindow(const EglWindow&) = delete;
  const EglWindow& operator=(const EglWindow&) = delete;

  [[nodiscard]] wl_surface* GetNativeSurface() const { return m_surface; }

  [[maybe_unused]] [[nodiscard]] bool SurfaceConfigured() const {
    return m_configured;
  };

  [[nodiscard]] int32_t GetWidth() const { return m_width; }
  [[nodiscard]] int32_t GetHeight() const { return m_height; }

  uint32_t GetFpsCounter();
  void DrawFps(uint8_t fps);

  bool ActivateSystemCursor(int32_t device, const std::string& kind);

  uint32_t m_fps_counter;

 private:
  struct shm_buffer {
    struct wl_buffer* buffer;
    void* shm_data;
    int busy;
  };

  size_t m_index;
  std::shared_ptr<Display> m_display;

  struct wl_surface* m_surface;
  struct wl_shell_surface* m_shell_surface;
  wl_egl_window* m_egl_window[kEngineInstanceCount]{};

  int32_t m_width;
  int32_t m_height;

  bool m_fullscreen;
  enum window_type m_type;
  std::string m_app_id;

  struct wl_surface* m_fps_surface;
  struct wl_subsurface* m_subsurface;
  struct shm_buffer m_fps_buffer;
  uint8_t m_fps_idx;
  uint8_t m_fps[20];

  struct shm_buffer m_buffers[2]{};
  struct wl_callback* m_callback;
  bool m_configured;

  int m_frame_sync;

  void toggle_fullscreen();

  static void buffer_release(void* data,
                             [[maybe_unused]] struct wl_buffer* buffer);

  static const struct wl_buffer_listener buffer_listener;

  static int create_shm_buffer(Display* display,
                               struct shm_buffer* buffer,
                               int width,
                               int height,
                               uint32_t format);

  static void handle_shell_ping(void* data,
                                struct wl_shell_surface* shell_surface,
                                uint32_t serial);

  static void handle_shell_configure(void* data,
                                     struct wl_shell_surface* shell_surface,
                                     uint32_t edges,
                                     int32_t width,
                                     int32_t height);

  static void handle_shell_popup_done(void* data,
                                      struct wl_shell_surface* shell_surface);

  static const struct wl_shell_surface_listener shell_surface_listener;

  static void handle_shell_configure_callback(void* data,
                                              struct wl_callback* callback,
                                              uint32_t time);

  static const struct wl_callback_listener shell_configure_callback_listener;

  [[maybe_unused]] static struct shm_buffer* next_buffer(EglWindow* window);

  static void paint_pixels_top(void* image,
                               int padding,
                               int width,
                               int height,
                               uint32_t time);

  static void paint_pixels_bottom(void* image,
                                  int padding,
                                  int width,
                                  int height,
                                  uint32_t time);

  static void paint_pixels(void* image,
                           int padding,
                           int width,
                           int height,
                           uint32_t time);

  static void redraw(void* data, struct wl_callback* callback, uint32_t time);

  static const struct wl_callback_listener frame_listener;
};
