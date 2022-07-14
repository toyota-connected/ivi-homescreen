
#pragma once

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

class FlutterView {
 public:
  FlutterView(Configuration::Config config,
              size_t index,
              const std::shared_ptr<Display>& display);
  ~FlutterView();
  void RunTasks();
  void Initialize();

  std::shared_ptr<WaylandWindow> GetEglWindow() { return m_wayland_window; }

  Backend* GetBackend() { return reinterpret_cast<Backend*>(m_backend.get()); }

  void DrawFps(long long end_time);

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

  struct {
    uint8_t output;
    uint32_t period;
    uint32_t counter;
    long long pretime;
  } m_fps{};
};