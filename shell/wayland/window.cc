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

#include <utility>

#include "display.h"
#include "engine.h"

WaylandWindow::WaylandWindow(const size_t index,
                             std::shared_ptr<Display> display,
                             const std::string& type,
                             wl_output* output,
                             const uint32_t output_index,
                             std::string app_id,
                             const bool fullscreen,
                             const int32_t width,
                             const int32_t height,
                             const double pixel_ratio,
                             const uint32_t activation_area_x,
                             const uint32_t activation_area_y,
                             const uint32_t activation_area_width,
                             const uint32_t activation_area_height,
                             Backend* backend,
                             const uint32_t ivi_surface_id)
    : m_index(index),
      m_display(std::move(display)),
      m_wl_output(output),
      m_output_index(output_index),
      m_flutter_engine(nullptr),
      m_pixel_ratio(pixel_ratio),
      m_backend(backend),
      m_ivi_surface_id(ivi_surface_id),
      m_fullscreen(fullscreen),
      m_geometry({width, height}),
      m_activation_area({activation_area_x, activation_area_y,
                         activation_area_width, activation_area_height}),
      m_window_size({width, height}),
      m_type(get_window_type(type)),
      m_app_id(std::move(app_id)) {  // disable vsync
  SPDLOG_TRACE("({}) + WaylandWindow()", m_index);

  m_base_surface = wl_compositor_create_surface(m_display->GetCompositor());
  wl_surface_add_listener(m_base_surface, &m_base_surface_listener, this);

#if ENABLE_IVI_SHELL_CLIENT
  auto ivi_application = m_display->GetIviApplication();
  if (ivi_application) {
    if (m_ivi_surface_id == 0) {
      spdlog::critical("IVI Surface ID not set");
      exit(EXIT_FAILURE);
    }
    m_ivi_surface = ivi_application_surface_create(
        ivi_application, m_ivi_surface_id, m_base_surface);
    if (m_ivi_surface == nullptr) {
      spdlog::error("Failed to create ivi_client_surface");
    }
    ivi_surface_add_listener(m_ivi_surface, &ivi_surface_listener, this);

    m_wait_for_configure = false;
  }
#elif defined(ENABLE_XDG_CLIENT)
  {
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
  }
#endif

  wl_surface_commit(m_base_surface);

#if ENABLE_AGL_SHELL_CLIENT
  switch (m_type) {
    case WINDOW_NORMAL:
      break;
    case WINDOW_BG:
      m_display->AglShellDoBackground(m_base_surface, 0);
      if (m_activation_area.x || m_activation_area.y ||
          m_activation_area.width || m_activation_area.height)
        m_display->AglShellDoSetupActivationArea(
            m_activation_area.x, m_activation_area.y, m_activation_area.width,
            m_activation_area.height, 0);
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
      spdlog::critical("Invalid surface role type supplied");
      assert(false);
  }
#endif

  // this makes the start-up from the beginning with the correction dimensions
  // like starting as maximized/fullscreen, rather than starting up as floating
  // width, height then performing a resize
  while (m_wait_for_configure) {
    wl_display_dispatch(m_display->GetDisplay());

    /* wait until xdg_surface::configure ACKs the new dimensions */
    if (m_wait_for_configure)
      continue;
  }

  m_backend->CreateSurface(m_index, m_base_surface, m_geometry.width,
                           m_geometry.height);

  // Register *after* initial geometry has settled so the event-thread
  // OnOutputResized path never observes a partially-constructed window.
  m_display->RegisterWindow(this);

  SPDLOG_TRACE("({}) - WaylandWindow()", m_index);
}

WaylandWindow::~WaylandWindow() {
  SPDLOG_TRACE("({}) + ~WaylandWindow()", m_index);

  // Unregister first so an in-flight wl_output.mode callback can't see
  // us mid-destruction; UnregisterWindow's lock pairs with the snapshot
  // copy in Display::NotifyOutputResized.
  if (m_display) {
    m_display->UnregisterWindow(this);
  }

  if (m_base_frame_callback)
    wl_callback_destroy(m_base_frame_callback);

#if ENABLE_IVI_SHELL_CLIENT
  if (m_ivi_surface)
    ivi_surface_destroy(m_ivi_surface);
#endif

#if ENABLE_XDG_CLIENT
  if (m_xdg_surface)
    xdg_surface_destroy(m_xdg_surface);

  if (m_xdg_toplevel)
    xdg_toplevel_destroy(m_xdg_toplevel);
#endif

  wl_surface_destroy(m_base_surface);

  SPDLOG_TRACE("({}) - ~WaylandWindow()", m_index);
}

void WaylandWindow::handle_base_surface_enter(void* data,
                                              struct wl_surface* /* surface */,
                                              struct wl_output* /* output */) {
  auto const* d = static_cast<WaylandWindow*>(data);

  const auto buffer_scale = d->m_display->GetBufferScale(d->m_output_index);

  const auto result =
      d->m_flutter_engine->SetPixelRatio(d->m_pixel_ratio * buffer_scale);
  if (result != kSuccess) {
    spdlog::error("Failed to set Flutter Engine Pixel Ratio");
  }
}

void WaylandWindow::handle_base_surface_leave(void* /* data */,
                                              struct wl_surface* /* surface */,
                                              struct wl_output* /* output */) {
  SPDLOG_TRACE("Leaving output");
}

const struct wl_surface_listener WaylandWindow::m_base_surface_listener = {
    .enter = handle_base_surface_enter,
    .leave = handle_base_surface_leave,
#if WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION
    .preferred_buffer_scale = nullptr,
#endif
#if WL_SURFACE_PREFERRED_BUFFER_TRANSFORM_SINCE_VERSION
    .preferred_buffer_transform = nullptr,
#endif
};

#if ENABLE_XDG_CLIENT
void WaylandWindow::handle_xdg_surface_configure(
    void* data,
    struct xdg_surface* xdg_surface,
    uint32_t serial) {
  auto* w = static_cast<WaylandWindow*>(data);
  xdg_surface_ack_configure(xdg_surface, serial);
  w->m_wait_for_configure = false;
}

const struct xdg_surface_listener WaylandWindow::xdg_surface_listener = {
    .configure = handle_xdg_surface_configure};
#endif

#if ENABLE_IVI_SHELL_CLIENT
void WaylandWindow::handle_ivi_surface_configure(
    void* data,
    struct ivi_surface* /* ivi_surface */,
    int32_t width,
    int32_t height) {
  auto* w = static_cast<WaylandWindow*>(data);

  if (width > 0 && height > 0) {
    if (w->m_fullscreen) {
      SPDLOG_DEBUG("Setting Fullscreen");
      const auto extents = w->m_display->GetVideoModeSize(w->m_output_index);
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

  w->m_backend->Resize(w->m_index, w->m_flutter_engine.get(),
                       w->m_geometry.width, w->m_geometry.height);

  w->m_wait_for_configure = false;
}

const struct ivi_surface_listener WaylandWindow::ivi_surface_listener = {
    .configure = handle_ivi_surface_configure};
#endif

#if ENABLE_XDG_CLIENT
void WaylandWindow::handle_toplevel_configure(
    void* data,
    struct xdg_toplevel* /* toplevel */,
    int32_t width,
    int32_t height,
    struct wl_array* states) {
  auto* w = static_cast<WaylandWindow*>(data);

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
      default:;
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
    // (0,0) configure = "client picks size". Downsize the client request
    // to fit the compositor's hint (configure_bounds if known, else the
    // output mode) so we don't render off-screen or get clipped on
    // compositors like tinywlr that don't enforce bounds at commit time.
    int32_t cap_w = w->m_configure_bounds.width;
    int32_t cap_h = w->m_configure_bounds.height;
    if (cap_w <= 0 || cap_h <= 0) {
      const auto out = w->m_display->GetVideoModeSize(w->m_output_index);
      cap_w = out.first;
      cap_h = out.second;
    }
    int32_t target_w = w->m_window_size.width;
    int32_t target_h = w->m_window_size.height;
    if (cap_w > 0 && target_w > cap_w) {
      target_w = cap_w;
    }
    if (cap_h > 0 && target_h > cap_h) {
      target_h = cap_h;
    }
    w->m_geometry.width = target_w;
    w->m_geometry.height = target_h;
  }

  w->m_backend->Resize(w->m_index, w->m_flutter_engine.get(),
                       w->m_geometry.width, w->m_geometry.height);
}

void WaylandWindow::handle_toplevel_configure_bounds(
    void* data,
    struct xdg_toplevel* /* toplevel */,
    int32_t width,
    int32_t height) {
  auto* w = static_cast<WaylandWindow*>(data);
  w->m_configure_bounds.width = width;
  w->m_configure_bounds.height = height;
}

void WaylandWindow::OnOutputResized(const size_t output_index,
                                    const int32_t new_w,
                                    const int32_t new_h) {
  if (output_index != m_output_index) {
    return;
  }
  // Only shrink — Weston (nested) emits a fresh wl_output.mode whenever
  // the host window is resized but does NOT re-send xdg_toplevel.configure,
  // so without this hook a 1024x768 surface stays oversized after the user
  // pulls Weston down to 1024x640 and Weston starts spamming events to
  // compensate ("Data too big for buffer"). Growth is the compositor's
  // call — leave that to the next configure.
  if (new_w <= 0 || new_h <= 0) {
    return;
  }
  int32_t target_w = m_geometry.width;
  int32_t target_h = m_geometry.height;
  if (target_w > new_w) {
    target_w = new_w;
  }
  if (target_h > new_h) {
    target_h = new_h;
  }
  if (target_w == m_geometry.width && target_h == m_geometry.height) {
    return;
  }
  m_geometry.width = target_w;
  m_geometry.height = target_h;
  m_backend->Resize(m_index, m_flutter_engine.get(), target_w, target_h);
}

void WaylandWindow::handle_toplevel_close(
    void* data,
    struct xdg_toplevel* /* xdg_toplevel */) {
  auto* w = static_cast<WaylandWindow*>(data);
  w->m_running = false;
}

const struct xdg_toplevel_listener WaylandWindow::xdg_toplevel_listener = {
    .configure = handle_toplevel_configure,
    .close = handle_toplevel_close,
#if defined(XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION)
    .configure_bounds = handle_toplevel_configure_bounds,
#endif
#if defined(XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
    .wm_capabilities = nullptr,
#endif
};
#endif

bool WaylandWindow::ActivateSystemCursor(int32_t device,
                                         const std::string& kind) const {
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

    const auto buffer_scale = m_display->GetBufferScale(m_output_index);

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
  }
  if (type == "BG") {
    return WINDOW_BG;
  }
  if (type == "PANEL_TOP") {
    return WINDOW_PANEL_TOP;
  }
  if (type == "PANEL_BOTTOM") {
    return WINDOW_PANEL_BOTTOM;
  }
  if (type == "PANEL_LEFT") {
    return WINDOW_PANEL_LEFT;
  }
  if (type == "PANEL_RIGHT") {
    return WINDOW_PANEL_RIGHT;
  }
  return WINDOW_NORMAL;
}
