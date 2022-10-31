
#include "compositor_surface.h"

#include <flutter/fml/logging.h>

#include <dlfcn.h>

#include <EGL/eglext.h>
#include <wayland-egl.h>
#include <utility>

#include "../wayland/display.h"
#include "../wayland/window.h"

CompositorSurface::~CompositorSurface() {
  if (type_ == CompositorSurface::egl) {
    wl_egl_window_destroy(wl_->egl_window);
  }
  wl_subsurface_destroy(subsurface_);
  wl_surface_destroy(wl_->surface);

  if (h_module_) {
    api_.de_initialize(api_.ctx);
    dlclose(h_module_);
  }
}

void CompositorSurface::InitApi() {
  api_.version = reinterpret_cast<COMP_SURF_API_VERSION_T*>(
      dlsym(h_module_, "comp_surf_version"));
  if (api_.version) {
    auto version = api_.version();
    if (version != kCompSurfExpectedInterfaceVersion) {
      FML_LOG(ERROR) << "Unexpected interface version: " << version;
      exit(1);
    }
  } else {
    goto invalid;
  }

  api_.loader = reinterpret_cast<COMP_SURF_API_LOAD_FUNCTIONS*>(
      dlsym(h_module_, "comp_surf_load_functions"));
  if (!api_.loader) {
    goto invalid;
  }
  api_.initialize = reinterpret_cast<COMP_SURF_API_INITIALIZE_T*>(
      dlsym(h_module_, "comp_surf_initialize"));
  if (!api_.initialize) {
    goto invalid;
  }
  api_.de_initialize = reinterpret_cast<COMP_SURF_API_DE_INITIALIZE_T*>(
      dlsym(h_module_, "comp_surf_de_initialize"));
  if (!api_.de_initialize) {
    goto invalid;
  }
  api_.run_task = reinterpret_cast<COMP_SURF_API_RUN_TASK_T*>(
      dlsym(h_module_, "comp_surf_run_task"));
  if (!api_.run_task) {
    goto invalid;
  }
  api_.resize = reinterpret_cast<COMP_SURF_API_RESIZE_T*>(
      dlsym(h_module_, "comp_surf_resize"));
  if (!api_.resize) {
    goto invalid;
  }
  return;

invalid:
  FML_LOG(ERROR) << "Invalid API";
  exit(1);
}

CompositorSurface::CompositorSurface(
    const std::shared_ptr<Display>& display,
    const std::shared_ptr<WaylandWindow>& window,
    void* h_module,
    std::string assets_path,
    CompositorSurface::PARAM_SURFACE_T type,
    CompositorSurface::PARAM_Z_ORDER_T z_order,
    CompositorSurface::PARAM_SYNC_T sync,
    int width,
    int height,
    int32_t x,
    int32_t y)
    : h_module_(h_module),
      assets_path_(std::move(assets_path)),
      type_(type),
      z_order_(z_order),
      sync_(sync),
      width_(width),
      height_(height),
      x_(x),
      y_(y) {
  InitApi();

  // Surface

  wl_ = std::make_unique<wl>();

  wl_->display = display->GetDisplay();
  wl_->surface = wl_compositor_create_surface(display->GetCompositor());
  surface_flutter_ = window->GetFlutterSurface();
  surface_base_ = window->GetBaseSurface();
  wl_->egl_display = nullptr;
  wl_->egl_window = nullptr;

  if (type == CompositorSurface::egl) {
    wl_->egl_display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR,
                                             display->GetDisplay(), nullptr);
    wl_->egl_window = wl_egl_window_create(wl_->surface, width, height);
    assert(wl_->egl_display);
    assert(wl_->egl_window);
  }

  // Sub-surface
  subsurface_ = wl_subcompositor_get_subsurface(display->GetSubCompositor(),
                                                wl_->surface,
                                                window->GetFlutterSurface());

  // Position
  wl_subsurface_set_position(subsurface_, x_, y_);

  // Z-Order
  if (z_order_ == CompositorSurface::PARAM_Z_ORDER_T::above) {
    wl_subsurface_place_above(subsurface_, window->GetFlutterSurface());
  } else if (z_order_ == CompositorSurface::PARAM_Z_ORDER_T::below) {
    wl_subsurface_place_below(subsurface_, window->GetFlutterSurface());
  }

  // Sync
  if (sync_ == CompositorSurface::PARAM_SYNC_T::sync) {
    wl_subsurface_set_sync(subsurface_);
  } else if (sync_ == CompositorSurface::PARAM_SYNC_T::de_sync) {
    wl_subsurface_set_desync(subsurface_);
  }

  // Initialize Plugin
  api_.ctx =
      api_.initialize("", width_, height_, wl_.get(), assets_path_.c_str(),
                      &CompositorSurface::Commit, this);
}

/*
 * Commit Surface stack
 * required for framecallback
 */
extern "C" void CompositorSurface::Commit(void *userdata) {
    auto obj = reinterpret_cast<CompositorSurface*>(userdata);
    wl_surface_commit(obj->wl_->surface);
    wl_surface_commit(obj->surface_flutter_);
    wl_surface_commit(obj->surface_base_);
}
