#pragma once

#include <wayland-client.h>
#include <wayland-egl.h>

#include "flutter_desktop_engine_state.h"
#include "flutter_homescreen.h"
#include "platform_views/platform_view.h"
#include "view/flutter_view.h"
#include "wayland/display.h"
#include "libnav_render.h"

class Display;

class FlutterView;

namespace nav_render_view_plugin {
class NavRenderSurface : public PlatformView, public flutter::Plugin {
 public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar* registrar,
                                    int32_t id,
                                    std::string viewType,
                                    int32_t direction,
                                    double top,
                                    double left,
                                    double width,
                                    double height,
                                    const std::vector<uint8_t>& params,
                                    std::string assetDirectory,
                                    FlutterDesktopEngineRef engine,
                                    PlatformViewAddListener addListener,
                                    PlatformViewRemoveListener removeListener,
                                    void* platform_view_context);

  NavRenderSurface(int32_t id,
                   std::string viewType,
                   int32_t direction,
                   double top,
                   double left,
                   double width,
                   double height,
                   const std::vector<uint8_t>& params,
                   std::string assetDirectory,
                   FlutterDesktopEngineState* state,
                   PlatformViewAddListener addListener,
                   PlatformViewRemoveListener removeListener,
                   void* platform_view_context);

  ~NavRenderSurface() override;

  // Disallow copy and assign.
  NavRenderSurface(const NavRenderSurface&) = delete;
  NavRenderSurface& operator=(const NavRenderSurface&) = delete;

 private:
  int32_t id_;
  FlutterView* view_;
  int32_t left_{};
  int32_t top_{};
  int32_t width_{};
  int32_t height_{};

//  nav_render_Context nav_render_context_;

  wl_display* display_;
  wl_surface* surface_;
  wl_surface* parent_surface_;
  wl_callback* callback_;
  wl_subsurface* subsurface_;

  EGLDisplay egl_display_;
  wl_egl_window* egl_window_;
  int buffer_size_ = 32;
  EGLContext egl_context_{};
  EGLConfig egl_config_{};
  GLuint programObject_{};
  EGLSurface egl_surface_{};

  void* platformViewsContext_;
  PlatformViewRemoveListener removeListener_;
  const std::string flutterAssetsPath_;

  void InitializeEGL();

  void InitializeScene();

  void DrawFrame(uint32_t time) const;

  void Resize(int32_t width, int32_t height);

  void SetOffset(int32_t left, int32_t top);

  static void on_resize(double width, double height, void* data);

  static void on_set_direction(int32_t direction, void* data);

  static void on_set_offset(double left, double top, void* data);

  static void on_touch(int32_t action,
                       int32_t point_count,
                       size_t point_data_size,
                       const double* point_data,
                       void* data);

  static void on_dispose(bool hybrid, void* data);

  static void on_frame(void* data, wl_callback* callback, uint32_t time);

  static const wl_callback_listener frame_listener;
  static const struct platform_view_listener platform_view_listener_;
};
}  // namespace nav_render_view_plugin
