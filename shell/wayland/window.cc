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
#include "logging/logging.h"

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

  // Assign the surface its role via the active compositor-protocol shell
  // (xdg / agl / ivi / simple).
  ivi::WindowConfig cfg;
  switch (m_type) {
    case WINDOW_BG:
      cfg.role = ivi::SurfaceRole::kBackground;
      break;
    case WINDOW_PANEL_TOP:
      cfg.role = ivi::SurfaceRole::kPanelTop;
      break;
    case WINDOW_PANEL_BOTTOM:
      cfg.role = ivi::SurfaceRole::kPanelBottom;
      break;
    case WINDOW_PANEL_LEFT:
      cfg.role = ivi::SurfaceRole::kPanelLeft;
      break;
    case WINDOW_PANEL_RIGHT:
      cfg.role = ivi::SurfaceRole::kPanelRight;
      break;
    case WINDOW_NORMAL:
    default:
      cfg.role = ivi::SurfaceRole::kNormal;
      break;
  }
  cfg.app_id = m_app_id;
  cfg.output = m_wl_output;
  cfg.output_index = m_output_index;
  cfg.fullscreen = m_fullscreen;
  cfg.width = m_geometry.width;
  cfg.height = m_geometry.height;
  cfg.ivi_surface_id = m_ivi_surface_id;
  cfg.on_configure = [this](const ivi::SurfaceConfigure& c) {
    // Refresh window-state flags from the compositor's configure (xdg only;
    // ivi/simple report all-false). These gate the sizing decision below.
    m_fullscreen = c.fullscreen;
    m_maximized = c.maximized;
    m_resize = c.resizing;
    m_activated = c.activated;
    const int32_t w = c.width;
    const int32_t h = c.height;
    if (w > 0 && h > 0) {
      if (!m_fullscreen && !m_maximized) {
        m_window_size.width = w;
        m_window_size.height = h;
      }
      m_geometry.width = w;
      m_geometry.height = h;
    } else if (!m_fullscreen && !m_maximized) {
      // (0,0) configure = "client picks size". Cap to the compositor's
      // configure_bounds hint (if known) or the output mode so we don't render
      // off-screen.
      int32_t cap_w = m_configure_bounds.width;
      int32_t cap_h = m_configure_bounds.height;
      if (cap_w <= 0 || cap_h <= 0) {
        const auto out = m_display->GetVideoModeSize(m_output_index);
        cap_w = out.first;
        cap_h = out.second;
      }
      int32_t target_w = m_window_size.width;
      int32_t target_h = m_window_size.height;
      if (cap_w > 0 && target_w > cap_w) {
        target_w = cap_w;
      }
      if (cap_h > 0 && target_h > cap_h) {
        target_h = cap_h;
      }
      m_geometry.width = target_w;
      m_geometry.height = target_h;
    }
    m_wait_for_configure = false;
    m_backend->Resize(m_index, m_flutter_engine.get(), m_geometry.width,
                      m_geometry.height);
  };
  cfg.on_close = [this]() { m_running = false; };

  auto& shell = m_display->ActiveShell();
  m_shell_surface = shell.CreateSurface(m_base_surface, cfg);

  // xdg-family surfaces gate startup on the first configure; ivi/simple do not.
  const auto shell_name = shell.Name();
  m_wait_for_configure = (shell_name == "xdg" || shell_name == "agl");

  wl_surface_commit(m_base_surface);

  // AGL window management (set_background / set_panel / activation region) must
  // follow the first commit. Only AglShell exposes a WindowManager facet.
  if (auto* wm = shell.WindowManager()) {
    switch (m_type) {
      case WINDOW_BG:
        wm->SetBackground(m_base_surface, 0);
        if (m_activation_area.x || m_activation_area.y ||
            m_activation_area.width || m_activation_area.height) {
          wm->SetActivationArea(m_activation_area.x, m_activation_area.y,
                                m_activation_area.width,
                                m_activation_area.height, 0);
        }
        break;
      case WINDOW_PANEL_TOP:
        wm->SetPanel(m_base_surface, ivi::SurfaceRole::kPanelTop, 0);
        break;
      case WINDOW_PANEL_BOTTOM:
        wm->SetPanel(m_base_surface, ivi::SurfaceRole::kPanelBottom, 0);
        break;
      case WINDOW_PANEL_LEFT:
        wm->SetPanel(m_base_surface, ivi::SurfaceRole::kPanelLeft, 0);
        break;
      case WINDOW_PANEL_RIGHT:
        wm->SetPanel(m_base_surface, ivi::SurfaceRole::kPanelRight, 0);
        break;
      default:
        break;
    }
  }

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

  // The role (xdg_surface/toplevel, ivi_surface, ...) is destroyed by
  // m_shell_surface's destructor.
  m_shell_surface.reset();

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
