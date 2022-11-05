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

#include "window.h"

#include <cstring>
#include <utility>

#include "constants.h"
#include "display.h"
#include "engine.h"

WaylandWindow::WaylandWindow(size_t index,
                             std::shared_ptr<Display> display,
                             const std::string& type,
                             wl_output* output,
                             std::string app_id,
                             bool fullscreen,
                             int32_t width,
                             int32_t height,
                             Backend* backend)
    : m_index(index),
      m_display(std::move(display)),
      m_wl_output(output),
      m_backend(backend),
      m_flutter_engine(nullptr),
      m_geometry({width, height}),
      m_window_size({width, height}),
      m_type(get_window_type(type)),
      m_app_id(std::move(app_id)),
      m_fullscreen(fullscreen) {  // disable vsync
  FML_DLOG(INFO) << "(" << m_index << ") + WaylandWindow()";

  m_fps_counter = 0;
  m_base_surface = wl_compositor_create_surface(m_display->GetCompositor());
  m_base_frame_callback = wl_surface_frame(m_base_surface);
  wl_callback_add_listener(m_base_frame_callback, &frame_listener_base_surface,
                           this);

  m_flutter_surface = wl_compositor_create_surface(m_display->GetCompositor());
  m_flutter_subsurface = wl_subcompositor_get_subsurface(
      m_display->GetSubCompositor(), m_flutter_surface, m_base_surface);
  m_flutter_frame_callback = wl_surface_frame(m_flutter_surface);
  wl_callback_add_listener(m_flutter_frame_callback,
                           &frame_listener_flutter_surface, this);

  m_xdg_surface =
      xdg_wm_base_get_xdg_surface(m_display->GetXdgWmBase(), m_base_surface);

  xdg_surface_add_listener(m_xdg_surface, &xdg_surface_listener, this);
  m_xdg_toplevel = xdg_surface_get_toplevel(m_xdg_surface);

  xdg_toplevel_add_listener(m_xdg_toplevel, &xdg_toplevel_listener, this);

  xdg_toplevel_set_app_id(m_xdg_toplevel, m_app_id.c_str());
  xdg_toplevel_set_title(m_xdg_toplevel, m_app_id.c_str());

  if (m_fullscreen)
    xdg_toplevel_set_fullscreen(m_xdg_toplevel, m_wl_output);

  m_wait_for_configure = true;
  wl_surface_commit(m_base_surface);
  wl_surface_commit(m_flutter_surface);

  switch (m_type) {
    case WINDOW_NORMAL:
      break;
    case WINDOW_BG:
      m_display->AglShellDoBackground(m_base_surface, 0);
      break;
    case WINDOW_PANEL_TOP:
      m_display->AglShellDoPanel(m_base_surface, AGL_SHELL_EDGE_TOP, 0);
      break;
    case WINDOW_PANEL_BOTTOM:
      m_display->AglShellDoPanel(m_base_surface, AGL_SHELL_EDGE_BOTTOM, 0);
      break;
    case WINDOW_PANEL_LEFT:
      m_display->AglShellDoPanel(m_base_surface, AGL_SHELL_EDGE_LEFT, 0);
      break;
    case WINDOW_PANEL_RIGHT:
      m_display->AglShellDoPanel(m_base_surface, AGL_SHELL_EDGE_RIGHT, 0);
      break;
    default:
      assert(!"Invalid surface role type supplied");
  }

  // this makes the start-up from the beginning with the correction dimensions
  // like starting as maximized/fullscreen, rather than starting up as floating
  // width, height then performing a resize
  while (m_wait_for_configure) {
    wl_display_dispatch(m_display->GetDisplay());

    /* wait until xdg_surface::configure acks the new dimensions */
    if (m_wait_for_configure)
      continue;

    m_backend->CreateSurface(m_index, m_base_surface, m_geometry.width,
                             m_geometry.height);
  }

  FML_DLOG(INFO) << "(" << m_index << ") - WaylandWindow()";
}

WaylandWindow::~WaylandWindow() {
  FML_DLOG(INFO) << "(" << m_index << ") + ~WaylandWindow()";

  if (m_base_frame_callback)
    wl_callback_destroy(m_base_frame_callback);

  if (m_xdg_surface)
    xdg_surface_destroy(m_xdg_surface);

  if (m_xdg_toplevel)
    xdg_toplevel_destroy(m_xdg_toplevel);

  wl_surface_destroy(m_base_surface);

  FML_DLOG(INFO) << "(" << m_index << ") - ~WaylandWindow()";
}

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

void WaylandWindow::handle_toplevel_configure(void* data,
                                              struct xdg_toplevel* toplevel,
                                              int32_t width,
                                              int32_t height,
                                              struct wl_array* states) {
  (void)toplevel;
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

void WaylandWindow::handle_toplevel_close(void* data,
                                          struct xdg_toplevel* xdg_toplevel) {
  (void)xdg_toplevel;
  auto* w = reinterpret_cast<WaylandWindow*>(data);
  w->m_running = false;
}

const struct xdg_toplevel_listener WaylandWindow::xdg_toplevel_listener = {
    handle_toplevel_configure,
    handle_toplevel_close,
};

const struct wl_callback_listener WaylandWindow::frame_listener_base_surface = {
    on_frame_base_surface};

void WaylandWindow::on_frame_base_surface(void* data,
                                          struct wl_callback* callback,
                                          uint32_t time) {
  (void)time;
  auto* window = reinterpret_cast<WaylandWindow*>(data);

  if (callback)
    wl_callback_destroy(callback);

  window->m_base_frame_callback = wl_surface_frame(window->m_base_surface);
  wl_callback_add_listener(window->m_base_frame_callback,
                           &frame_listener_base_surface, window);

  window->m_fps_counter++;
  window->m_fps_counter++;

  wl_surface_commit(window->m_base_surface);
}

const struct wl_callback_listener
    WaylandWindow::frame_listener_flutter_surface = {on_frame_flutter_surface};

void WaylandWindow::on_frame_flutter_surface(void* data,
                                             struct wl_callback* callback,
                                             uint32_t time) {
  (void)time;
  auto* window = reinterpret_cast<WaylandWindow*>(data);

  if (callback)
    wl_callback_destroy(callback);

  window->m_flutter_frame_callback =
      wl_surface_frame(window->m_flutter_surface);
  wl_callback_add_listener(window->m_flutter_frame_callback,
                           &frame_listener_flutter_surface, window);

  wl_surface_commit(window->m_flutter_surface);
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
        m_flutter_engine->SetWindowSize(m_geometry.height, m_geometry.width);
    if (result != kSuccess) {
      FML_LOG(ERROR) << "Failed to set Flutter Engine Window Size";
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

void WaylandWindow::CommitSurfaces() {
  wl_surface_commit(m_flutter_surface);
  wl_surface_commit(m_base_surface);
}
