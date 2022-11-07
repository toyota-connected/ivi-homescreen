
#include "flutter_view.h"

#include <memory>
#include <utility>

#if defined(BUILD_BACKEND_WAYLAND_EGL)
#include "backend/wayland_egl.h"
#elif defined(BUILD_BACKEND_WAYLAND_VULKAN)
#include "backend/wayland_vulkan.h"
#endif
#include "configuration/configuration.h"
#include "engine.h"
#ifdef ENABLE_PLUGIN_GSTREAMER_EGL
#include "static_plugins/gstreamer_egl/gstreamer_egl.h"
#endif
#ifdef ENABLE_PLUGIN_COMP_SURF
#include "compositor_surface.h"
#endif
#include "wayland/display.h"
#include "wayland/window.h"

FlutterView::FlutterView(Configuration::Config config,
                         size_t index,
                         const std::shared_ptr<Display>& display)
    : m_config(std::move(config)),
      m_index(index),
      m_wayland_display(display)
#ifdef ENABLE_PLUGIN_TEXT_INPUT
      ,
      m_text_input(std::make_shared<TextInput>())
#endif
{
#if defined(BUILD_BACKEND_WAYLAND_EGL)
  m_backend = std::make_shared<WaylandEglBackend>(
      display->GetDisplay(), m_config.debug_backend, kEglBufferSize);
#elif defined(BUILD_BACKEND_WAYLAND_VULKAN)
  m_backend = std::make_shared<WaylandVulkanBackend>(
      display->GetDisplay(), m_config.view.width, m_config.view.height,
      m_config.debug_backend);
#endif
  m_wayland_window = std::make_shared<WaylandWindow>(
      m_index, display, m_config.view.window_type,
      m_wayland_display->GetWlOutput(m_config.view.wl_output_index),
      m_config.app_id, m_config.view.fullscreen, m_config.view.width,
      m_config.view.height, m_backend.get());

#ifdef ENABLE_TEXTURE_TEST_EGL
  m_texture_test_egl = std::make_unique<TextureTestEgl>(this);
#endif
#ifdef ENABLE_TEXTURE_NAVI_RENDER_EGL
  m_texture_navi = std::make_unique<TextureNaviRender>(this);
#endif
}

FlutterView::~FlutterView() = default;

void FlutterView::Initialize() {
  std::vector<const char*> m_command_line_args_c;
  m_command_line_args_c.reserve(m_config.view.vm_args.size());
  m_command_line_args_c.push_back(m_config.app_id.c_str());
  for (const auto& arg : m_config.view.vm_args) {
    m_command_line_args_c.push_back(arg.c_str());
  }

  m_flutter_engine = std::make_shared<Engine>(
      this, m_index, m_command_line_args_c, m_config.view.bundle_path,
      m_config.view.accessibility_features);
  m_flutter_engine->Run(pthread_self());

  if (!m_flutter_engine->IsRunning()) {
    FML_LOG(ERROR) << "Failed to Run Engine";
    exit(EXIT_FAILURE);
  }

  // Engine events are decoded by surface pointer
  m_wayland_display->SetEngine(m_wayland_window->GetBaseSurface(),
                               m_flutter_engine.get());
  m_wayland_window->SetEngine(m_flutter_engine);

  FML_DLOG(INFO) << "(" << m_index << ") Engine running...";

#ifdef ENABLE_TEXTURE_TEST_EGL
  m_texture_test_egl->SetEngine(m_flutter_engine);
#endif
#ifdef ENABLE_PLUGIN_TEXT_INPUT
  m_text_input->SetEngine(m_flutter_engine);
  m_wayland_display->SetTextInput(m_wayland_window->GetBaseSurface(),
                                  m_text_input.get());
#endif
#ifdef ENABLE_TEXTURE_NAVI_RENDER_EGL
  m_texture_navi->SetEngine(m_flutter_engine);
#endif

  // init the fps output option.
  m_fps.output = 0;
  m_fps.period = 1;
  m_fps.counter = 0;

  if (m_config.view.fps_output_console) {
    m_fps.output |= 0x01;
  }
  if (m_config.view.fps_output_overlay) {
    m_fps.output |= 0x02;
  }
  if (m_config.view.fps_output_frequency) {
    m_fps.period = m_config.view.fps_output_frequency;
  }

  if (0 < m_fps.output) {
    if (0 >= m_fps.period) {
      m_fps.period = 1;
    }

    m_fps.period *= (1000 / 16);
    m_fps.pretime = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
  }
}

void FlutterView::RunTasks() {
  m_flutter_engine->RunTask();

#ifdef ENABLE_TEXTURE_TEST_EGL
  if (m_texture_test_egl) {
    m_texture_test_egl->Draw(m_texture_test_egl.get());
  }
#endif
#ifdef ENABLE_TEXTURE_NAVI_RENDER_EGL
  if (m_texture_navi) {
    m_texture_navi->Draw(m_texture_navi.get());
  }
#endif
#ifdef ENABLE_PLUGIN_COMP_SURF
  for (auto const& surface : m_comp_surf) {
    surface.second->RunTask();
  }
#endif
}

// calc and output the FPS
void FlutterView::DrawFps(long long end_time) {
  if (0 < m_fps.output) {
    m_fps.counter++;

    if (m_fps.period <= m_fps.counter) {
      auto fps_loop = (m_fps.counter * 1000) / (end_time - m_fps.pretime);
      auto fps_redraw = (m_wayland_window->GetFpsCounter() * 1000) /
                        (end_time - m_fps.pretime);

      m_fps.counter = 0;
      m_fps.pretime = end_time;

      if (0 < (m_fps.output & 0x01)) {
        if (0 < (m_fps.output & 0x01)) {
          FML_LOG(INFO) << "(" << m_index << ") FPS = " << fps_loop << " "
                        << fps_redraw;
        }
      }
    }
  }
}

#ifdef ENABLE_PLUGIN_COMP_SURF
size_t FlutterView::CreateSurface(void* h_module,
                                  const std::string& assets_path,
                                  const std::string& cache_folder,
                                  const std::string& misc_folder,
                                  CompositorSurface::PARAM_SURFACE_T type,
                                  CompositorSurface::PARAM_Z_ORDER_T z_order,
                                  CompositorSurface::PARAM_SYNC_T sync,
                                  int width,
                                  int height,
                                  int32_t x,
                                  int32_t y) {
  auto index = static_cast<int64_t>(m_comp_surf.size());
  m_comp_surf[index] = std::make_unique<CompositorSurface>(
      index, m_wayland_display, m_wayland_window, h_module, assets_path,
      cache_folder, misc_folder, type, z_order, sync, width, height, x, y, this);

  m_comp_surf[index]->InitializePlugin();
  return index;
}

void FlutterView::DisposeSurface(int64_t key) {
  m_comp_surf[key]->StopFrames();
  m_comp_surf[key]->Dispose(m_comp_surf[key].get());
  m_comp_surf[key].reset();

  m_comp_surf.erase(key);
}

void* FlutterView::GetSurfaceContext(int64_t index) {
  void* res = nullptr;
  if (m_comp_surf.find(index) != m_comp_surf.end()) {
    res = m_comp_surf[index]->GetContext();
  }
  return res;
}
#endif