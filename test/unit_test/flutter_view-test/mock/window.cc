// Copyright 2020 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "wayland/window.h"

#include <utility>

#include "engine.h"
#include "wayland/display.h"

WaylandWindow::WaylandWindow(size_t index,
                             std::shared_ptr<Display> display,
                             const std::string& type,
                             wl_output* output,
                             uint32_t output_index,
                             std::string app_id,
                             bool fullscreen,
                             int32_t width,
                             int32_t height,
                             double pixel_ratio,
                             uint32_t /* activation_area_x */,
                             uint32_t /* activation_area_y */,
                             Backend* backend,
                             uint32_t ivi_surface_id)
    : m_index(index),
      m_display(std::move(display)),
      m_wl_output(output),
      m_output_index(output_index),
      m_backend(backend),
      m_flutter_engine(nullptr),
      m_geometry({width, height}),
      m_window_size({width, height}),
      m_pixel_ratio(pixel_ratio),
      m_type(get_window_type(type)),
      m_app_id(std::move(app_id)),
      m_ivi_surface_id(ivi_surface_id),
      m_fullscreen(fullscreen) {  // disable vsync
  /* Delete implementation */
}

WaylandWindow::~WaylandWindow() {
  SPDLOG_TRACE("({}) + ~WaylandWindow()", m_index);

  if (m_base_frame_callback)
    wl_callback_destroy(m_base_frame_callback);

  if (m_ivi_surface)
    ivi_surface_destroy(m_ivi_surface);

  if (m_xdg_surface)
    xdg_surface_destroy(m_xdg_surface);

  if (m_xdg_toplevel)
    xdg_toplevel_destroy(m_xdg_toplevel);

  wl_surface_destroy(m_base_surface);

  SPDLOG_TRACE("({}) - ~WaylandWindow()", m_index);
}

void WaylandWindow::handle_base_surface_enter(void* data,
                                              struct wl_surface* /* surface */,
                                              struct wl_output* /* output */) {
  auto* d = static_cast<WaylandWindow*>(data);

  auto buffer_scale = d->m_display->GetBufferScale(d->m_output_index);

  auto result =
      d->m_flutter_engine->SetPixelRatio(d->m_pixel_ratio * buffer_scale);
  if (result != kSuccess) {
    spdlog::error("Failed to set Flutter Engine Pixel Ratio");
  }
}

void WaylandWindow::handle_base_surface_leave(void* /* data */,
                                              struct wl_surface* /* surface */,
                                              struct wl_output* /* output */) {
  SPDLOG_DEBUG("Leaving output");
}

const struct wl_surface_listener WaylandWindow::m_base_surface_listener = {
    .enter = handle_base_surface_enter,
    .leave = handle_base_surface_leave,
};

void WaylandWindow::handle_xdg_surface_configure(
    void* data,
    struct xdg_surface* xdg_surface,
    uint32_t serial) {
  auto* w = reinterpret_cast<WaylandWindow*>(data);
  xdg_surface_ack_configure(xdg_surface, serial);
  w->m_wait_for_configure = false;
}

const struct xdg_surface_listener WaylandWindow::xdg_surface_listener = {
    .configure = handle_xdg_surface_configure};

void WaylandWindow::handle_ivi_surface_configure(
    void* data,
    struct ivi_surface* /* ivi_surface */,
    int32_t width,
    int32_t height) {
  auto* w = reinterpret_cast<WaylandWindow*>(data);

  if (width > 0 && height > 0) {
    if (w->m_fullscreen) {
      SPDLOG_DEBUG("Setting Fullscreen");
      auto extents = w->m_display->GetVideoModeSize(w->m_output_index);
      width = extents.first;
      height = extents.second;
    }
    if (!w->m_fullscreen && !w->m_maximized) {
      w->m_window_size.width = width;
      w->m_window_size.height = height;
    }
    w->m_geometry.width = width;
    w->m_geometry.height = height;
  }

  w->m_backend->Resize(w->m_index, w->m_flutter_engine, w->m_geometry.width,
                       w->m_geometry.height);

  w->m_wait_for_configure = false;
}

const struct ivi_surface_listener WaylandWindow::ivi_surface_listener = {
    .configure = handle_ivi_surface_configure};

void WaylandWindow::handle_toplevel_configure(
    void* data,
    struct xdg_toplevel* /* toplevel */,
    int32_t width,
    int32_t height,
    struct wl_array* states) {
  auto* w = reinterpret_cast<WaylandWindow*>(data);

  w->m_fullscreen = false;
  w->m_maximized = false;
  w->m_resize = false;
  w->m_activated = false;

  const uint32_t* state;
  WL_ARRAY_FOR_EACH(state, states, const uint32_t*) {
    switch (*state) {
      case XDG_TOPLEVEL_STATE_FULLSCREEN:
        w->m_fullscreen = true;
        break;
      case XDG_TOPLEVEL_STATE_MAXIMIZED:
        w->m_maximized = true;
        break;
      case XDG_TOPLEVEL_STATE_RESIZING:
        w->m_resize = true;
        break;
      case XDG_TOPLEVEL_STATE_ACTIVATED:
        w->m_activated = true;
        break;
    }
  }

  if (width > 0 && height > 0) {
    if (!w->m_fullscreen && !w->m_maximized) {
      w->m_window_size.width = width;
      w->m_window_size.height = height;
    }
    w->m_geometry.width = width;
    w->m_geometry.height = height;

  } else if (!w->m_fullscreen && !w->m_maximized) {
    w->m_geometry.width = w->m_window_size.width;
    w->m_geometry.height = w->m_window_size.height;
  }

  w->m_backend->Resize(w->m_index, w->m_flutter_engine, w->m_geometry.width,
                       w->m_geometry.height);
}

void WaylandWindow::handle_toplevel_close(
    void* data,
    struct xdg_toplevel* /* xdg_toplevel */) {
  auto* w = reinterpret_cast<WaylandWindow*>(data);
  w->m_running = false;
}

const struct xdg_toplevel_listener WaylandWindow::xdg_toplevel_listener = {
    handle_toplevel_configure,
    handle_toplevel_close,
};

const struct wl_callback_listener WaylandWindow::m_base_surface_frame_listener =
    {on_frame_base_surface};

void WaylandWindow::on_frame_base_surface(void* data,
                                          struct wl_callback* callback,
                                          uint32_t /* time */) {
  auto* window = reinterpret_cast<WaylandWindow*>(data);

  if (callback)
    wl_callback_destroy(callback);

  window->m_base_frame_callback = wl_surface_frame(window->m_base_surface);
  wl_callback_add_listener(window->m_base_frame_callback,
                           &m_base_surface_frame_listener, window);

  window->m_fps_counter++;
  window->m_fps_counter++;

  wl_surface_commit(window->m_base_surface);
}

uint32_t WaylandWindow::GetFpsCounter() {
  uint32_t fps_counter = m_fps_counter;
  m_fps_counter = 0;

  return fps_counter;
}

bool WaylandWindow::ActivateSystemCursor(int32_t device,
                                         const std::string& kind) {
  return m_display->ActivateSystemCursor(device, kind);
}

void WaylandWindow::SetEngine(const std::shared_ptr<Engine>& engine) {
  m_flutter_engine = engine;
  if (m_flutter_engine) {
    auto result =
        m_flutter_engine->SetWindowSize(static_cast<size_t>(m_geometry.height),
                                        static_cast<size_t>(m_geometry.width));
    if (result != kSuccess) {
      spdlog::error("Failed to set Flutter Engine Window Size");
    }

    auto buffer_scale = m_display->GetBufferScale(m_output_index);

    result = m_flutter_engine->SetPixelRatio(m_pixel_ratio * buffer_scale);
    if (result != kSuccess) {
      spdlog::error("Failed to set Flutter Engine Pixel Ratio");
    }
  }
}

WaylandWindow::window_type WaylandWindow::get_window_type(
    const std::string& type) {
  if (type == "NORMAL") {
    return WINDOW_NORMAL;
  } else if (type == "BG") {
    return WINDOW_BG;
  } else if (type == "PANEL_TOP") {
    return WINDOW_PANEL_TOP;
  } else if (type == "PANEL_BOTTOM") {
    return WINDOW_PANEL_BOTTOM;
  } else if (type == "PANEL_LEFT") {
    return WINDOW_PANEL_LEFT;
  } else if (type == "PANEL_RIGHT") {
    return WINDOW_PANEL_RIGHT;
  }
  return WINDOW_NORMAL;
}
