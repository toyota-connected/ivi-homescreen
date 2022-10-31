
#pragma once

#include <memory>

#include "configuration/configuration.h"
#include "flutter/fml/macros.h"

#include <EGL/egl.h>
#include <wayland-client.h>

#include "compositor_surface_api.h"

class Display;

class WaylandWindow;

class CompositorSurface {
 public:
  enum PARAM_SURFACE_T {
    egl,
    vulkan,
  };

  enum PARAM_Z_ORDER_T {
    above,
    below,
  };

  enum PARAM_SYNC_T {
    sync,
    de_sync,
  };

  CompositorSurface(const std::shared_ptr<Display>& wayland_display,
                    const std::shared_ptr<WaylandWindow>& wayland_window,
                    void* h_module,
                    std::string assets_path,
                    CompositorSurface::PARAM_SURFACE_T type,
                    CompositorSurface::PARAM_Z_ORDER_T z_order,
                    CompositorSurface::PARAM_SYNC_T sync,
                    int width,
                    int height,
                    int32_t x,
                    int32_t y);

  ~CompositorSurface();

  [[nodiscard]] void* GetContext() const { return api_.ctx; }

  void RunTask() const { api_.run_task(api_.ctx); }

  static void Commit(void *userdata);

 private:
  typedef struct {
    struct wl_display* display;
    struct wl_surface* surface;
    EGLDisplay egl_display;
    struct wl_egl_window* egl_window;
  } wl;

  void InitApi();

  std::unique_ptr<wl> wl_;
  wl_surface* surface_flutter_;
  wl_surface* surface_base_;

  wl_subsurface* subsurface_;
  void* h_module_;
  std::string assets_path_;
  PARAM_SURFACE_T type_;
  PARAM_Z_ORDER_T z_order_;
  PARAM_SYNC_T sync_;
  int width_;
  int height_;
  int32_t x_;
  int32_t y_;

  struct {
    COMP_SURF_API_VERSION_T* version{};

    COMP_SURF_API_CONTEXT_T* ctx{};
    COMP_SURF_API_LOAD_FUNCTIONS* loader{};
    COMP_SURF_API_INITIALIZE_T* initialize{};
    COMP_SURF_API_DE_INITIALIZE_T* de_initialize{};
    COMP_SURF_API_RUN_TASK_T* run_task{};
    COMP_SURF_API_RESIZE_T* resize{};

  } api_;
};
