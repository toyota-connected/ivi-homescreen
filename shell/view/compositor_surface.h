
#pragma once

#include <memory>

#include "configuration/configuration.h"
#include "flutter/fml/macros.h"

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
                    std::string  assets_path,
                    CompositorSurface::PARAM_SURFACE_T type,
                    CompositorSurface::PARAM_Z_ORDER_T z_order,
                    CompositorSurface::PARAM_SYNC_T sync,
                    int width,
                    int height,
                    int32_t x,
                    int32_t y);

  ~CompositorSurface();

  void* GetContext() { return api_.ctx; }

  void Draw() { api_.render(api_.ctx, 0); }

 private:
  typedef struct {
    struct wl_display* display;
    struct wl_surface* surface;
  } wl;

  void InitApi();

  std::unique_ptr<wl> wl_;
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
    COMP_SURF_API_RENDER_T* render{};
    COMP_SURF_API_RESIZE_T* resize{};

  } api_;
};
