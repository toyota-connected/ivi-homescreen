
#pragma once

#include <map>
#include <memory>

#include "configuration/configuration.h"
#include "flutter/fml/macros.h"
#include "wayland/window.h"
#ifdef ENABLE_TEXTURE_TEST_EGL
#include "textures/test_egl/texture_test_egl.h"
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT
#include "static_plugins/text_input/text_input.h"
#endif
#ifdef ENABLE_TEXTURE_NAVI_RENDER_EGL
#include "textures/navi_render_egl/texture_navi_render_egl.h"
#endif
#ifdef ENABLE_PLUGIN_COMP_SURF
#include "compositor_surface.h"
#endif

class Display;
class Engine;
class Backend;
class PlatformChannel;
class WaylandWindow;
#if defined(BUILD_BACKEND_WAYLAND_EGL)
class WaylandEglBackend;
#elif defined(BUILD_BACKEND_WAYLAND_VULKAN)
class WaylandVulkanBackend;
#endif
#ifdef ENABLE_PLUGIN_COMP_SURF
class CompositorSurface;
#endif

class FlutterView {
 public:
  FlutterView(Configuration::Config config,
              size_t index,
              const std::shared_ptr<Display>& display);
  ~FlutterView();

  /**
  * @brief Run Tasks
  * @return void
  * @relation
  * wayland, flutter
  */
  void RunTasks();

  /**
  * @brief Initialize
  * @return void
  * @relation
  * wayland, flutter
  */
  void Initialize();

  /**
  * @brief Get Egl Window
  * @return shared_ptr<WaylandWindow>
  * @retval Egl Window
  * @relation
  * wayland, flutter
  */
  std::shared_ptr<WaylandWindow> GetEglWindow() { return m_wayland_window; }

  /**
  * @brief Get Backend
  * @return Backend*
  * @retval Backend pointer
  * @relation
  * wayland, flutter
  */
  Backend* GetBackend() { return reinterpret_cast<Backend*>(m_backend.get()); }

  [[nodiscard]] uint64_t GetIndex() const { return m_index; }

  /**
  * @brief Draw FPS to calc and output
  * @param[in] end_time End time
  * @return void
  * @relation
  * wayland, flutter
  */
  void DrawFps(long long end_time);

#ifdef ENABLE_PLUGIN_COMP_SURF
  size_t CreateSurface(void* h_module,
                       const std::string& assets_path,
                       const std::string& cache_path,
                       const std::string& misc_path,
                       CompositorSurface::PARAM_SURFACE_T type,
                       CompositorSurface::PARAM_Z_ORDER_T z_order,
                       CompositorSurface::PARAM_SYNC_T sync,
                       int width,
                       int height,
                       int32_t x,
                       int32_t y);

  void DisposeSurface(int64_t index);

  typedef std::map<int64_t, std::unique_ptr<CompositorSurface>> surface_array_t;
  surface_array_t m_comp_surf;

  void* GetSurfaceContext(int64_t index);
#endif

  FML_DISALLOW_COPY_AND_ASSIGN(FlutterView);

 private:
#if defined(BUILD_BACKEND_WAYLAND_EGL)
  std::shared_ptr<WaylandEglBackend> m_backend;
#elif defined(BUILD_BACKEND_WAYLAND_VULKAN)
  std::shared_ptr<WaylandVulkanBackend> m_backend;
#endif
  std::shared_ptr<Display> m_wayland_display;
  std::shared_ptr<WaylandWindow> m_wayland_window;
  std::shared_ptr<Engine> m_flutter_engine;
  const Configuration::Config m_config;
  std::shared_ptr<PlatformChannel> m_platform_channel;
  size_t m_index;

#ifdef ENABLE_TEXTURE_TEST_EGL
  std::unique_ptr<TextureTestEgl> m_texture_test_egl;
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT
  std::shared_ptr<TextInput> m_text_input;
#endif
#ifdef ENABLE_TEXTURE_NAVI_RENDER_EGL
  std::unique_ptr<TextureNaviRender> m_texture_navi;
#endif

  struct {
    uint8_t output;
    uint32_t period;
    uint32_t counter;
    long long pretime;
  } m_fps{};
};