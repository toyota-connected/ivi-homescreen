
#include "compositor_surface.h"

#include <filesystem>

#include <flutter/fml/paths.h>

#include <dlfcn.h>

#include <EGL/eglext.h>
#include <wayland-egl.h>
#include <utility>

#include "../utils.h"
#include "../view/flutter_view.h"
#include "../wayland/display.h"

CompositorSurface::CompositorSurface(
    int64_t /* key */,
    const std::shared_ptr<Display>& display,
    const std::shared_ptr<WaylandWindow>& window,
    void* h_module,
    std::string assets_path,
    const std::string& cache_folder,
    const std::string& misc_folder,
    CompositorSurface::PARAM_SURFACE_T type,
    CompositorSurface::PARAM_Z_ORDER_T z_order,
    CompositorSurface::PARAM_SYNC_T sync,
    int width,
    int height,
    int32_t x,
    int32_t y)
    : m_h_module(h_module),
      m_assets_path(std::move(assets_path)),
      m_cache_path(GetFilePath(cache_folder.c_str())),
      m_misc_path(GetFilePath(misc_folder.c_str())),
      m_type(type),
      m_z_order(z_order),
      m_sync(sync),
      width_(width),
      height_(height),
      m_origin_x(x),
      m_origin_y(y),
      m_context(nullptr),
      m_callback(nullptr) {
  // API
  init_api(this);

  // Surface
  m_wl.display = display->GetDisplay();
  m_wl.surface = wl_compositor_create_surface(display->GetCompositor());
  m_wl.egl_display = nullptr;
  m_wl.egl_window = nullptr;
  m_wl.width = static_cast<uint32_t>(width);
  m_wl.height = static_cast<uint32_t>(height);

  auto parent_surface = window->GetBaseSurface();

  if (type == CompositorSurface::egl) {
    m_wl.egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR,
                                             display->GetDisplay(), nullptr);
    m_wl.egl_window = wl_egl_window_create(m_wl.surface, width, height);
    assert(m_wl.egl_display);
    assert(m_wl.egl_window);
  }

  // Sub-surface
  m_subsurface = wl_subcompositor_get_subsurface(display->GetSubCompositor(),
                                                 m_wl.surface, parent_surface);
  assert(m_subsurface);

  // Position
  wl_subsurface_set_position(m_subsurface, m_origin_x, m_origin_y);

  // Z-Order
  if (m_z_order == CompositorSurface::PARAM_Z_ORDER_T::above) {
    wl_subsurface_place_above(m_subsurface, parent_surface);
  } else if (m_z_order == CompositorSurface::PARAM_Z_ORDER_T::below) {
    wl_subsurface_place_below(m_subsurface, parent_surface);
  }

  // Sync
  if (m_sync == CompositorSurface::PARAM_SYNC_T::sync) {
    wl_subsurface_set_sync(m_subsurface);
  } else if (m_sync == CompositorSurface::PARAM_SYNC_T::de_sync) {
    wl_subsurface_set_desync(m_subsurface);
  }
}

void CompositorSurface::Dispose(void* userdata) {
  auto* obj = (CompositorSurface*)userdata;

  obj->m_api.de_initialize(obj->m_context);
  obj->m_context = nullptr;

  if (obj->m_subsurface) {
    wl_subsurface_destroy(obj->m_subsurface);
    obj->m_subsurface = nullptr;
  }

  if (obj->m_wl.egl_window) {
    wl_egl_window_destroy(obj->m_wl.egl_window);
    obj->m_wl.egl_window = nullptr;
  }

  if (obj->m_wl.surface) {
    wl_surface_destroy(obj->m_wl.surface);
    obj->m_wl.surface = nullptr;
  }

  if (obj->m_h_module) {
    dlclose(obj->m_h_module);
    obj->m_h_module = nullptr;
  }
}

void CompositorSurface::init_api(CompositorSurface* obj) {
  obj->m_api.version = reinterpret_cast<COMP_SURF_API_VERSION_T*>(
      dlsym(obj->m_h_module, "comp_surf_version"));
  if (obj->m_api.version) {
    auto version = obj->m_api.version();
    if (version != kCompSurfExpectedInterfaceVersion) {
      spdlog::critical("Unexpected interface version: 0x{:x}", version);
      exit(1);
    }
  } else {
    goto invalid;
  }

  obj->m_api.loader = reinterpret_cast<COMP_SURF_API_LOAD_FUNCTIONS*>(
      dlsym(obj->m_h_module, "comp_surf_load_functions"));
  if (!obj->m_api.loader) {
    goto invalid;
  }
  obj->m_api.initialize = reinterpret_cast<COMP_SURF_API_INITIALIZE_T*>(
      dlsym(obj->m_h_module, "comp_surf_initialize"));
  if (!obj->m_api.initialize) {
    goto invalid;
  }
  obj->m_api.de_initialize = reinterpret_cast<COMP_SURF_API_DE_INITIALIZE_T*>(
      dlsym(obj->m_h_module, "comp_surf_de_initialize"));
  if (!obj->m_api.de_initialize) {
    goto invalid;
  }
  obj->m_api.run_task = reinterpret_cast<COMP_SURF_API_RUN_TASK_T*>(
      dlsym(obj->m_h_module, "comp_surf_run_task"));
  if (!obj->m_api.run_task) {
    goto invalid;
  }
  obj->m_api.draw_frame = reinterpret_cast<COMP_SURF_API_DRAW_FRAME_T*>(
      dlsym(obj->m_h_module, "comp_surf_draw_frame"));
  if (!obj->m_api.draw_frame) {
    goto invalid;
  }
  obj->m_api.resize = reinterpret_cast<COMP_SURF_API_RESIZE_T*>(
      dlsym(obj->m_h_module, "comp_surf_resize"));
  if (!obj->m_api.resize) {
    goto invalid;
  }
  return;

invalid:
  spdlog::critical("Invalid API");
  exit(1);
}

std::string CompositorSurface::GetFilePath(const char* folder) {
  auto path = fml::paths::JoinPaths({Utils::GetConfigHomePath(), folder});

  if (!std::filesystem::is_directory(path) || !std::filesystem::exists(path)) {
    if (!std::filesystem::create_directories(path)) {
      spdlog::critical("GetCachePath create_directories failed: {}", path);
      exit(EXIT_FAILURE);
    }
  }

  return path;
}

void CompositorSurface::InitializePlugin() {
  m_context =
      m_api.initialize("", width_, height_, &m_wl, m_assets_path.c_str(),
                       m_cache_path.c_str(), m_misc_path.c_str());
  StartFrames();
}

void CompositorSurface::StartFrames() {
  if (m_callback)
    wl_callback_destroy(m_callback);
  m_callback = nullptr;
  on_frame(this, m_callback, 0);
}

void CompositorSurface::StopFrames() {
  if (m_callback)
    wl_callback_destroy(m_callback);
  m_callback = nullptr;
}

void CompositorSurface::on_frame(void* data,
                                 struct wl_callback* callback,
                                 uint32_t time) {
  auto obj = reinterpret_cast<CompositorSurface*>(data);

  obj->m_callback = nullptr;

  if (callback)
    wl_callback_destroy(callback);

  obj->m_api.draw_frame(obj->m_context, time);

  obj->m_callback = wl_surface_frame(obj->m_wl.surface);
  wl_callback_add_listener(obj->m_callback, &CompositorSurface::frame_listener,
                           data);

  wl_subsurface_set_position(obj->m_subsurface, obj->m_origin_x,
                             obj->m_origin_y);

  wl_surface_commit(obj->m_wl.surface);
}

const struct wl_callback_listener CompositorSurface::frame_listener = {
    .done = on_frame};
