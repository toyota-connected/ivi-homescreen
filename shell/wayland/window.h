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

class Backend;

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
                const std::string& type,
                wl_output* output,
                std::string app_id,
                bool fullscreen,
                int32_t width,
                int32_t height,
                Backend* backend);

  ~WaylandWindow();

  WaylandWindow(const WaylandWindow&) = delete;

  const WaylandWindow& operator=(const WaylandWindow&) = delete;

  /**
  * @brief Set Engine
  * @param[in] engine Engine
  * @return void
  * @relation
  * wayland
  */
  void SetEngine(const std::shared_ptr<Engine>& engine);

  /**
  * @brief Get Fps Counter
  * @return uint32_t
  * @retval Frames Per Second counter.
  * @relation
  * wayland
  */
  uint32_t GetFpsCounter();

  bool ActivateSystemCursor(int32_t device, const std::string& kind);

  /**
  * @brief Get Base Surface
  * @return wl_surface*
  * @retval Base surface
  * @relation
  * wayland
  */
  wl_surface* GetBaseSurface() { return m_base_surface; }

  uint32_t m_fps_counter;

  /**
  * @brief Get window_type
  * @param[in] type Window type
  * @return window_type
  * @retval WINDOW_NORMAL
  * @retval WINDOW_BG
  * @retval WINDOW_PANEL_TOP
  * @retval WINDOW_PANEL_BOTTOM
  * @retval WINDOW_PANEL_LEFT
  * @retval WINDOW_PANEL_RIGHT
  * @relation
  * wayland, agl-shell
  */
  static window_type get_window_type(const std::string& type);

 private:
  size_t m_index;
  std::shared_ptr<Display> m_display;
  wl_output* m_wl_output;
  std::shared_ptr<Engine> m_flutter_engine;
  struct wl_surface* m_base_surface{};
  std::shared_ptr<Backend> m_backend;
  bool m_wait_for_configure{};

  bool m_fullscreen{};
  bool m_maximized{};
  MAYBE_UNUSED bool m_resize{};
  MAYBE_UNUSED bool m_activated{};
  MAYBE_UNUSED bool m_running{};
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

  struct wl_callback* m_base_frame_callback;

  static const struct xdg_surface_listener xdg_surface_listener;

  /**
  * @brief Response to configure event
  * @param[in] data Pointer to WaylandWindow type
  * @param[in] xdg_surface Surfaces in the domain of xdg-shell
  * @param[in] serial Serial of the configure event
  * @return void
  * @relation
  * wayland
  */
  static void handle_xdg_surface_configure(void* data,
                                           struct xdg_surface* xdg_surface,
                                           uint32_t serial);

  static const struct xdg_toplevel_listener xdg_toplevel_listener;

  /**
  * @brief Response to configure event
  * @param[in] data Pointer to WaylandWindow type
  * @param[in] toplevel No use
  * @param[in] width Width
  * @param[in] height Height
  * @param[in] states Dynamic array for checking xdg_toplevel_state
  * @return void
  * @relation
  * wayland
  */
  static void handle_toplevel_configure(void* data,
                                        struct xdg_toplevel* toplevel,
                                        int32_t width,
                                        int32_t height,
                                        struct wl_array* states);

  /**
  * @brief Close event
  * @param[in] data Pointer to WaylandWindow type
  * @param[in] xdg_toplevel No use
  * @return void
  * @relation
  * wayland
  */
  static void handle_toplevel_close(void* data,
                                    struct xdg_toplevel* xdg_toplevel);

  static void on_frame_base_surface(void* data,
                                    struct wl_callback* callback,
                                    uint32_t time);

  static const struct wl_callback_listener frame_listener_base_surface;
};
