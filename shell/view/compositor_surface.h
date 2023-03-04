
#pragma once

#include <memory>

#include "configuration/configuration.h"
#include "flutter/fml/macros.h"

#include <EGL/egl.h>
#include <wayland-client.h>

#include "compositor_surface_api.h"

class Display;

class FlutterView;

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

  CompositorSurface(int64_t key,
                    const std::shared_ptr<Display>& wayland_display,
                    const std::shared_ptr<WaylandWindow>& wayland_window,
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
                    int32_t y,
                    const FlutterView* view);

  ~CompositorSurface() = default;

  void InitializePlugin();

  [[nodiscard]] void* GetContext() const { return m_context; }

  void RunTask() const { m_api.run_task(m_context); }

  static void Dispose(void* userdata);

  void StartFrames();
  void StopFrames();

  static std::string GetFilePath(const char* folder);

 private:
  typedef struct {
    struct wl_display* display;
    struct wl_surface* surface;
    EGLDisplay egl_display;
    struct wl_egl_window* egl_window;
    uint32_t width;
    uint32_t height;
  } wl;

  wl m_wl{};

  struct wl_callback* m_callback;
  const FlutterView* m_view;

  wl_subsurface* m_subsurface;
  void* m_h_module;
  std::string m_assets_path;
  std::string m_cache_path;
  std::string m_misc_path;
  [[maybe_unused]] PARAM_SURFACE_T m_type;
  PARAM_Z_ORDER_T m_z_order;
  PARAM_SYNC_T m_sync;
  int width_;
  int height_;
  int32_t m_origin_x;
  int32_t m_origin_y;
  int64_t m_key;

  struct {
    COMP_SURF_API_VERSION_T* version{};
    COMP_SURF_API_LOAD_FUNCTIONS* loader{};
    COMP_SURF_API_INITIALIZE_T* initialize{};
    COMP_SURF_API_DE_INITIALIZE_T* de_initialize{};
    COMP_SURF_API_RUN_TASK_T* run_task{};
    COMP_SURF_API_DRAW_FRAME_T* draw_frame{};
    COMP_SURF_API_RESIZE_T* resize{};
  } m_api;

  COMP_SURF_API_CONTEXT_T* m_context{};

  static void init_api(CompositorSurface* obj);
  static void on_frame(void* data, struct wl_callback* callback, uint32_t time);
  static const struct wl_callback_listener frame_listener;
};
