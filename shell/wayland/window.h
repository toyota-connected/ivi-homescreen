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

#include "backend/backend.h"
#include "wayland/shell/wayland_shell.h"

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
                uint32_t output_index,
                std::string app_id,
                bool fullscreen,
                int32_t width,
                int32_t height,
                double pixel_ratio,
                uint32_t activation_area_x,
                uint32_t activation_area_y,
                uint32_t activation_area_width,
                uint32_t activation_area_height,
                Backend* backend,
                uint32_t ivi_surface_id);

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
   * @brief activate a system cursor
   * @param[in] device Device
   * @param[in] kind Kind of a cursor
   * @return bool
   * @retval true Success
   * @retval false Failure
   * @relation
   * platform
   */
  [[nodiscard]] bool ActivateSystemCursor(int32_t device,
                                          const std::string& kind) const;

  /**
   * @brief Get Base Surface
   * @return wl_surface*
   * @retval Base surface
   * @relation
   * wayland
   */
  [[nodiscard]] wl_surface* GetBaseSurface() const { return m_base_surface; }

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

  /**
   * @brief GetSize
   * @retval std::pair<int32_t, int32_t> width, height
   * @relation
   * wayland
   */
  std::pair<int32_t, int32_t> GetSize() {
    return std::pair<int32_t, int32_t>{m_geometry.width, m_geometry.height};
  }

  // Called from Display::NotifyOutputResized when wl_output.mode reports
  // new dimensions for output index @p output_index. Runs on the
  // wayland event thread. If this window is bound to that output and
  // its current geometry no longer fits, shrink to the new mode and
  // re-Resize the backend.
  void OnOutputResized(size_t output_index, int32_t new_w, int32_t new_h);

 private:
  size_t m_index;
  std::shared_ptr<Display> m_display;
  wl_output* m_wl_output;
  uint32_t m_output_index;
  std::shared_ptr<Engine> m_flutter_engine;
  double m_pixel_ratio;
  struct wl_surface* m_base_surface{};
  // Non-owning. The backend (Backend) is owned by the FlutterView that created
  // this window and outlives it; the ctor receives a raw Backend*. Storing it
  // in a shared_ptr here would create a SECOND, independent ownership of the
  // same object (FlutterView owns the first) and double-free it on teardown.
  Backend* m_backend{};
  bool m_wait_for_configure{};

  uint32_t m_ivi_surface_id;
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
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
  } m_activation_area;
  struct {
    int32_t width;
    int32_t height;
  } m_window_size{};

  // Latest hint from xdg_toplevel.configure_bounds (output area minus
  // struts). 0 = no hint received.
  struct {
    int32_t width;
    int32_t height;
  } m_configure_bounds{};

  enum window_type m_type;
  std::string m_app_id;

  // The surface's role (xdg toplevel / ivi surface / simple-shell surface),
  // created and owned by the active compositor-protocol shell. Its internal
  // listener forwards configure/close through the WindowConfig callbacks.
  std::unique_ptr<ivi::ShellSurface> m_shell_surface;

  struct wl_callback* m_base_frame_callback{};

  static const struct wl_surface_listener m_base_surface_listener;

  /**
   * @brief Handle enter base surface event
   * @param[in,out] data Data of type Display
   * @param[in] surface No use
   * @param[in] output Output
   * @return void
   * @relation
   * wayland
   */
  static void handle_base_surface_enter(void* data,
                                        struct wl_surface* surface,
                                        struct wl_output* output);

  /**
   * @brief Handle leave base surface event
   * @param[in,out] data Data of type Display
   * @param[in] surface No use
   * @param[in] output Output
   * @return void
   * @relation
   * wayland
   */
  static void handle_base_surface_leave(void* data,
                                        struct wl_surface* surface,
                                        struct wl_output* output);

  /**
   * @brief handler for frame event of a base surface
   * @param[in] data Pointer to WaylandWindow type
   * @param[in] callback a callback for frame event
   * @param[in] time No use
   * @return void
   * @relation
   * wayland
   */
  static void on_frame_base_surface(void* data,
                                    struct wl_callback* callback,
                                    uint32_t time);

  static const struct wl_callback_listener m_base_surface_frame_listener;
};
