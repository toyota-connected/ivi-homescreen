/*
 * Copyright 2021-2022 Toyota Connected North America
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

#include "backend/backend.h"
#include "xdg-shell-client-protocol.h"

// workaround for Wayland macro not compiling in C++
#define WL_ARRAY_FOR_EACH(pos, array, type)                             \
  for (pos = (type)(array)->data;                                       \
       (const char*)pos < ((const char*)(array)->data + (array)->size); \
       (pos)++)

class Display;
class Engine;

class WaylandWindow {
 public:
  // a normal surface role is a regular application; a window_bg, window_panel
  // are part of the client shell UI for AGL; for window_panel only the height
  // has any meaning, while window_bg will literally be the entire output
  enum window_type {
    WINDOW_NORMAL,
    WINDOW_BG,
    WINDOW_PANEL_TOP,
    WINDOW_PANEL_BOTTOM,
    WINDOW_PANEL_LEFT,
    WINDOW_PANEL_RIGHT
  };

  WaylandWindow(size_t index,
                std::shared_ptr<Display> display,
                struct wl_surface* base_surface,
                enum window_type type,
                std::string app_id,
                bool fullscreen,
                int32_t width,
                int32_t height,
                Backend* backend);

  ~WaylandWindow();
  WaylandWindow(const WaylandWindow&) = delete;
  const WaylandWindow& operator=(const WaylandWindow&) = delete;

  void SetEngine(const std::shared_ptr<Engine>& engine);

  wl_surface* GetNativeSurface() { return m_base_surface; }

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
  std::shared_ptr<Engine> m_flutter_engine;
  struct wl_surface* m_base_surface;
  std::shared_ptr<Backend> m_backend;
  bool m_wait_for_configure{};

  bool m_fullscreen{};
  bool m_maximized{};
  bool m_resize{};
  bool m_activated{};
  bool m_running{};
  struct {
    int32_t width;
    int32_t height;
  } m_geometry;
  struct {
    int32_t width;
    int32_t height;
  } m_window_size{};

  enum window_type m_type;
  std::string m_app_id;

  struct xdg_surface* m_xdg_surface;
  struct xdg_toplevel* m_xdg_toplevel;
  struct wl_surface* m_fps_surface;
  struct wl_subsurface* m_subsurface;
  struct shm_buffer m_fps_buffer {};
  uint8_t m_fps_idx;
  uint8_t m_fps[20]{};

  struct shm_buffer m_buffers[2]{};
  struct wl_callback* m_callback;

  int m_frame_sync;

  static void buffer_release(void* data,
                             [[maybe_unused]] struct wl_buffer* buffer);

  static const struct wl_buffer_listener buffer_listener;

  static int create_shm_buffer(Display* display,
                               struct shm_buffer* buffer,
                               int width,
                               int height,
                               uint32_t format);

  static const struct xdg_surface_listener xdg_surface_listener;

  static void handle_xdg_surface_configure(void* data,
                                           struct xdg_surface* xdg_surface,
                                           uint32_t serial);

  static const struct xdg_toplevel_listener xdg_toplevel_listener;

  static void handle_toplevel_configure(void* data,
                                        struct xdg_toplevel* toplevel,
                                        int32_t width,
                                        int32_t height,
                                        struct wl_array* states);

  static void handle_toplevel_close(void* data,
                                    struct xdg_toplevel* xdg_toplevel);

  [[maybe_unused]] static struct shm_buffer* next_buffer(WaylandWindow* window);

  static void redraw(void* data, struct wl_callback* callback, uint32_t time);

  static const struct wl_callback_listener frame_listener;
};
