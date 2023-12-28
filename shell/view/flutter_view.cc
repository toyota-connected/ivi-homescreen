// @copyright Copyright (c) 2022 Woven Alpha, Inc.
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
#include "plugins/gstreamer_egl/gstreamer_egl.h"
#endif
#ifdef ENABLE_PLUGIN_COMP_SURF
#include "compositor_surface.h"
#endif

#include <platform/homescreen/key_event_handler.h>
#include <platform/homescreen/text_input_plugin.h>
#include <plugins/audioplayers_linux/include/audioplayers_linux/audioplayers_linux_plugin_c_api.h>
#include <plugins/cloud_firestore/include/cloud_firestore/cloud_firestore_plugin_c_api.h>
#include <plugins/firebase_auth/include/firebase_auth/firebase_auth_plugin_c_api.h>
#include <plugins/firebase_core/include/firebase_core/firebase_core_plugin_c_api.h>
#include <plugins/firebase_storage/include/firebase_storage/firebase_storage_plugin_c_api.h>
#include <plugins/url_launcher/include/url_launcher/url_launcher_plugin_c_api.h>

#include "spdlog/fmt/bundled/chrono.h"
#include "wayland/display.h"
#include "wayland/window.h"

extern void SetUpCommonEngineState(FlutterDesktopEngineState* state,
                                   FlutterView* view);

FlutterView::FlutterView(Configuration::Config config,
                         const size_t index,
                         const std::shared_ptr<Display>& display)
    : m_wayland_display(display),
      m_config(std::move(config)),
      m_index(index)
{
#if defined(BUILD_BACKEND_WAYLAND_EGL)
  m_backend = std::make_shared<WaylandEglBackend>(
      display->GetDisplay(), m_config.view.width, m_config.view.height,
      m_config.debug_backend, kEglBufferSize);
#if defined(ENABLE_TEXTURE_EGL)
  TextureEgl::GetInstance().SetView(this);
#endif
#elif defined(BUILD_BACKEND_WAYLAND_VULKAN)
  m_backend = std::make_shared<WaylandVulkanBackend>(
      display->GetDisplay(), m_config.view.width, m_config.view.height,
      m_config.debug_backend);
#endif

  SPDLOG_DEBUG("Width: {}, Height: {}", m_config.view.width,
               m_config.view.height);

  m_wayland_window = std::make_shared<WaylandWindow>(
      m_index, display, m_config.view.window_type,
      m_wayland_display->GetWlOutput(m_config.view.wl_output_index),
      m_config.view.wl_output_index, m_config.app_id, m_config.view.fullscreen,
      m_config.view.width, m_config.view.height, m_config.view.pixel_ratio,
      m_config.view.activation_area_x, m_config.view.activation_area_y,
      m_backend.get(), m_config.view.ivi_surface_id);

  m_state = std::make_unique<FlutterDesktopViewControllerState>();
  m_state->view = this;
  m_state->view_wrapper = std::make_unique<FlutterDesktopView>();
  m_state->view_wrapper->view = this;

  m_state->engine_state = std::make_unique<FlutterDesktopEngineState>();
  // is set after engine is initialized
  // m_state->engine = m_flutter_engine.get();
  m_state->engine_state->view_controller = m_state.get();

  SetUpCommonEngineState(m_state->engine_state.get(), this);

  // Set up the keyboard handlers
  auto internal_plugin_messenger =
      m_state->engine_state->internal_plugin_registrar->messenger();
  m_state->keyboard_hook_handlers.push_back(
      std::make_unique<flutter::KeyEventHandler>(internal_plugin_messenger));
  m_state->keyboard_hook_handlers.push_back(
      std::make_unique<flutter::TextInputPlugin>(internal_plugin_messenger));
  m_wayland_display->SetViewControllerState(m_state->engine_state->view_controller);

  RegisterPlugins(m_state->engine_state.get());
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

  m_state->engine = m_flutter_engine.get();

  m_flutter_engine->Run(m_state->engine_state.get());

  if (!m_flutter_engine->IsRunning()) {
    spdlog::critical("Failed to Run Engine");
    exit(EXIT_FAILURE);
  }

  // Update for Binary Messenger
  m_state->engine_state->flutter_engine = m_flutter_engine->GetFlutterEngine();
  m_state->engine_state->platform_task_runner =
      m_flutter_engine->GetPlatformTaskRunner();

  // update view
  m_state->view = m_state->view_wrapper->view = this;

  // Engine events are decoded by surface pointer
  m_wayland_display->SetEngine(m_wayland_window->GetBaseSurface(),
                               m_flutter_engine.get());
  m_wayland_window->SetEngine(m_flutter_engine);

  SPDLOG_DEBUG("({}) Engine running...", m_index);

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
    m_fps.pre_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
  }
}

void FlutterView::RunTasks() {
  m_flutter_engine->RunTask();

#ifdef ENABLE_TEXTURE_EGL
  TextureEgl::GetInstance().Draw();
  TextureEgl::GetInstance().RunTask();
#endif

#ifdef ENABLE_PLUGIN_COMP_SURF
  for (auto const& surface : m_comp_surf) {
    surface.second->RunTask();
  }
#endif

  m_pointer_events++;
  if (m_pointer_events % kPointerEventModulus == 0) {
    m_flutter_engine->SendPointerEvents();
  }
}

// calc and output the FPS
void FlutterView::DrawFps(long long end_time) {
  if (0 < m_fps.output) {
    m_fps.counter++;

    if (m_fps.period <= m_fps.counter) {
      auto fps_loop = (m_fps.counter * 1000) / (end_time - m_fps.pre_time);
      auto fps_redraw = (m_wayland_window->GetFpsCounter() * 1000) /
                        (end_time - m_fps.pre_time);

      m_fps.counter = 0;
      m_fps.pre_time = end_time;

      if (0 < (m_fps.output & 0x01)) {
        if (0 < (m_fps.output & 0x01)) {
          spdlog::info("({}) FPS = {} {}", m_index, fps_loop, fps_redraw);
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
  const auto tStart = std::chrono::steady_clock::now();

  auto index = static_cast<int64_t>(m_comp_surf.size());
  m_comp_surf[index] = std::make_unique<CompositorSurface>(
      index, m_wayland_display, m_wayland_window, h_module, assets_path,
      cache_folder, misc_folder, type, z_order, sync, width, height, x, y);

  m_comp_surf[index]->InitializePlugin();

  const auto tEnd = std::chrono::steady_clock::now();
  const auto tDiff =
      std::chrono::duration<double, std::milli>(tEnd - tStart).count();
  spdlog::info("comp surf init: {}", static_cast<float>(tDiff));

  return static_cast<size_t>(index);
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

#ifdef ENABLE_PLUGIN_COMP_REGION
void FlutterView::ClearRegion(const std::string& type) const {
  // A NULL wl_region causes the pending input/opaque region to be set to empty.
  if (type == "input") {
    wl_surface_set_input_region(m_wayland_window->GetBaseSurface(), nullptr);
  } else if (type == "opaque") {
    wl_surface_set_opaque_region(m_wayland_window->GetBaseSurface(), nullptr);
  }
}

void FlutterView::SetRegion(
    const std::string& type,
    const std::vector<CompositorRegionPlugin::REGION_T>& regions) const {
  const auto compositor = m_wayland_display->GetCompositor();
  const auto base_region = wl_compositor_create_region(compositor);

  for (auto const& region : regions) {
    SPDLOG_DEBUG("Set Region: type: {}, x: {}, y: {}, width: {}, height: {}",
                 type, region.x, region.y, region.width, region.height);
    wl_region_add(base_region, region.x, region.y, region.width, region.height);
  }

  if (type == "input") {
    wl_surface_set_input_region(m_wayland_window->GetBaseSurface(),
                                base_region);
  } else if (type == "opaque") {
    wl_surface_set_opaque_region(m_wayland_window->GetBaseSurface(),
                                 base_region);
  }
  // Setting the pending input/opaque region has copy semantics,
  // and the wl_region object can be destroyed immediately.
  wl_region_destroy(base_region);
}
#endif

void FlutterView::RegisterPlugins(FlutterDesktopEngineRef engine) {
  (void)engine;
#if defined(ENABLE_PLUGIN_AUDIOPLAYERS_LINUX)
  AudioPlayersLinuxPluginCApiRegisterWithRegistrar(
      FlutterDesktopGetPluginRegistrar(engine, ""));
#endif
#if defined(ENABLE_PLUGIN_URL_LAUNCHER)
  UrlLauncherPluginCApiRegisterWithRegistrar(
      FlutterDesktopGetPluginRegistrar(engine, ""));
#endif
#if defined(ENABLE_PLUGIN_FIREBASE_CORE)
  FirebaseCorePluginCApiRegisterWithRegistrar(
      FlutterDesktopGetPluginRegistrar(engine, ""));
#endif
#if defined(ENABLE_PLUGIN_FIREBASE_STORAGE)
  FirebaseStoragePluginCApiRegisterWithRegistrar(
      FlutterDesktopGetPluginRegistrar(engine, ""));
#endif
#if defined(ENABLE_PLUGIN_FIREBASE_AUTH)
  FirebaseAuthPluginCApiRegisterWithRegistrar(
      FlutterDesktopGetPluginRegistrar(engine, ""));
#endif
#if defined(ENABLE_PLUGIN_CLOUD_FIRESTORE)
  CloudFirestorePluginCApiRegisterWithRegistrar(
      FlutterDesktopGetPluginRegistrar(engine, ""));
#endif
}
