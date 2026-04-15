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

#if BUILD_BACKEND_HEADLESS_EGL
#include "backend/headless/headless.h"
#elif BUILD_BACKEND_WAYLAND_DRM
#include "backend/wayland_drm/wayland_drm.h"
#elif BUILD_BACKEND_WAYLAND_EGL
#include "backend/wayland_egl/wayland_egl.h"
#elif BUILD_BACKEND_WAYLAND_VULKAN
#include "backend/wayland_vulkan/wayland_vulkan.h"
#endif
#include <key_event_handler.h>
#include <text_input_plugin.h>

#include "configuration/configuration.h"
#include "engine.h"

// TODO(kerberjg): these need to go
#ifdef ENABLE_PLUGIN_GSTREAMER_EGL
#include "plugins/gstreamer_egl/gstreamer_egl.h"
#endif
#ifdef ENABLE_PLUGIN_COMP_SURF
#include "compositor_surface.h"
#endif

#if ENABLE_PLUGINS
#include <generated_plugin_registrant.h>
#include "public/plugin/homescreen_plugin.h"
#include "public/tools/shared_library.h"

// NOTE: this is for static plugins
extern void GetStaticFlutterPlugins(FlutterDesktopEngineRef engine);
#endif

#include "wayland/display.h"
#include "wayland/window.h"

extern void SetUpCommonEngineState(FlutterDesktopEngineState* state,
                                   FlutterView* view);

FlutterView::FlutterView(Configuration::Config config,
                         const size_t index,
                         const std::shared_ptr<Display>& display)
    : m_wayland_display(display), m_config(std::move(config)), m_index(index) {
#if BUILD_BACKEND_HEADLESS_EGL
  m_backend = std::make_shared<HeadlessBackend>(
      m_config.view.width.value_or(kDefaultViewWidth),
      m_config.view.height.value_or(kDefaultViewHeight),
      m_config.debug_backend.value_or(false), kEglBufferSize);
#elif BUILD_BACKEND_WAYLAND_DRM
  m_backend = std::make_shared<WaylandDrmBackend>(
      display->GetDisplay(), m_config.view.width.value_or(kDefaultViewWidth),
      m_config.view.height.value_or(kDefaultViewHeight),
      m_config.debug_backend.value_or(false), kEglBufferSize);
#elif BUILD_BACKEND_WAYLAND_EGL
  m_backend = std::make_shared<WaylandEglBackend>(
      display->GetDisplay(), m_config.view.width.value_or(kDefaultViewWidth),
      m_config.view.height.value_or(kDefaultViewHeight),
      m_config.debug_backend.value_or(false), kEglBufferSize);
#elif BUILD_BACKEND_WAYLAND_VULKAN
  m_backend = std::make_shared<WaylandVulkanBackend>(
      display->GetDisplay(), m_config.view.width.value_or(kDefaultViewWidth),
      m_config.view.height.value_or(kDefaultViewHeight),
      m_config.debug_backend.value_or(false));
#endif

  SPDLOG_DEBUG("Width: {}, Height: {}",
               m_config.view.width.value_or(kDefaultViewWidth),
               m_config.view.height.value_or(kDefaultViewWidth));

  m_wayland_window = std::make_shared<WaylandWindow>(
      m_index, display, m_config.view.window_type,
      m_wayland_display->GetWlOutput(m_config.view.wl_output_index.value_or(0)),
      m_config.view.wl_output_index.value_or(0), m_config.app_id,
      m_config.view.fullscreen.value_or(false),
      m_config.view.width.value_or(kDefaultViewWidth),
      m_config.view.height.value_or(kDefaultViewWidth),
      m_config.view.pixel_ratio.value_or(kDefaultPixelRatio),
      m_config.view.activation_area_x, m_config.view.activation_area_y,
      m_config.view.activation_area_width, m_config.view.activation_area_height,
      m_backend.get(), m_config.view.ivi_surface_id.value_or(0));

  m_state = std::make_unique<FlutterDesktopViewControllerState>();
  m_state->view = this;
  m_state->view_wrapper = std::make_unique<FlutterDesktopView>();
  m_state->view_wrapper->view = this;

  m_state->engine_state = std::make_unique<FlutterDesktopEngineState>();
  m_state->engine_state->view_controller = m_state.get();

  // Set the flutter assets folder
  std::filesystem::path path = m_config.view.bundle_path;
  path /= kBundleFlutterAssets;
  m_state->engine_state->flutter_asset_directory = path.generic_string();

  SetUpCommonEngineState(m_state->engine_state.get(), this);

  // Set up the keyboard handlers
  auto internal_plugin_messenger =
      m_state->engine_state->internal_plugin_registrar->messenger();
  m_state->keyboard_hook_handlers.push_back(
      std::make_unique<flutter::KeyEventHandler>(internal_plugin_messenger));
  m_state->keyboard_hook_handlers.push_back(
      std::make_unique<flutter::TextInputPlugin>(internal_plugin_messenger));
  m_wayland_display->SetViewControllerState(
      m_state->engine_state->view_controller);

  /*
   * Load & Register plugins
   */
#if ENABLE_PLUGINS
  RegisterPlugins();
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

  /// AccessKit Wrapper
  m_accessibility_tree = std::make_shared<AccessibilityTree>();
  m_state->engine_state->accessibility_tree = m_accessibility_tree.get();

  m_flutter_engine = std::make_shared<Engine>(
      this, m_index, m_command_line_args_c, m_config.view.bundle_path,
      m_config.view.accessibility_features.value_or(0));

  m_state->engine = m_flutter_engine.get();

  m_flutter_engine->Run(m_state->engine_state.get());

  if (!m_flutter_engine->IsRunning()) {
    spdlog::critical("Failed to Run Engine");
    exit(EXIT_FAILURE);
  }

  // notify display update
  FlutterEngineDisplay display{};
  display.struct_size = sizeof(FlutterEngineDisplay);
  display.display_id = 1;
  display.single_display = true;
  display.refresh_rate =
      m_wayland_display->GetRefreshRate(static_cast<uint32_t>(m_index));
  auto [width, height] = m_wayland_window->GetSize();
  display.width = static_cast<size_t>(width);
  display.height = static_cast<size_t>(height);
  display.device_pixel_ratio = m_flutter_engine->GetPixelRatio();
  LibFlutterEngine->NotifyDisplayUpdate(m_flutter_engine->GetFlutterEngine(),
                                        kFlutterEngineDisplaysUpdateTypeStartup,
                                        &display, 1);

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
}

void FlutterView::RunTasks() {
  m_flutter_engine->RunTask();

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

/*
 * Plugin system
 */

#if ENABLE_PLUGINS
void FlutterView::RegisterPlugins() {
  auto engine = m_state->engine_state.get();
  auto registrar = engine->plugin_registrar.get();

  const bool static_first = true;  // TODO: get from app args
  std::map<std::string, FlutterPluginRegisterCallback> static_plugin_handles;

  // Get list of plugins to load from
  // `.desktop-homescreen/.flutter-plugins-dependencies` (JSON)
  std::ifstream plugin_config_file(m_config.view.bundle_path + "/" +
                                   kPluginConfig);

  if (!plugin_config_file.is_open()) {
    spdlog::warn(
        "Plugin config file not found at {}. No dynamic plugins will be "
        "loaded.",
        m_config.view.bundle_path + "/" + kPluginConfig  //
    );
    return;
  }

  // Get list of plugins to load for given app
  std::vector<std::string> plugins_to_load;
  {
    std::stringstream buffer;
    buffer << plugin_config_file.rdbuf();
    plugin_config_file.close();

    rapidjson::Document doc;
    doc.Parse(buffer.str().c_str());

    if (                                     //
        doc.IsObject()                       //
        && doc.HasMember("dependencyGraph")  //
        && doc["dependencyGraph"].IsArray()  //
    ) {
      // List plugins
      for (const auto& dep : doc["dependencyGraph"].GetArray()) {
        if (dep.IsObject() && dep.HasMember("name")) {
          std::string plugin_name = dep["name"].GetString();
          plugins_to_load.push_back(plugin_name);
        } else {
          spdlog::error(  //
              "Plugin config file is not in expected format: "
              "each dependency must be an object with a 'name' field. "
              "Skipping plugin entry."  //
          );
        }
      }
    }
  }

  // debug: print plugins to load
  spdlog::debug("Plugins to load:");
  for (const auto& plugin_name : plugins_to_load) {
    spdlog::debug("  {}", plugin_name);
  }

  // Get list of available static plugins
  std::map<const char*, FlutterPluginRegisterCallback>
      available_static_plugins = GetStaticFlutterPlugins();

  // debug: print available static plugins
  spdlog::debug("Available static plugins:");
  for (const auto& [plugin_name, _] : available_static_plugins) {
    spdlog::debug("  {}", plugin_name);
  }

  for (const auto& plugin_name : plugins_to_load) {
    spdlog::debug("Loading plugin: {}", plugin_name);
    FlutterPluginRegisterCallback register_func = nullptr;

    if (static_first) {
      // Try to find static plugin first
      auto it = available_static_plugins.find(plugin_name.c_str());
      if (it != available_static_plugins.end()) {
        register_func = it->second;
        spdlog::debug("...Found static plugin!");
      } else {
        // ...then try dynamic
        spdlog::debug("...Static plugin not found, loading dynamically... ");
        register_func = LoadDynamicPlugin(plugin_name);
      }
    } else {
      // Try to find dynamic plugin first
      register_func = LoadDynamicPlugin(plugin_name);
      if (!register_func) {
        spdlog::debug("...Dynamic plugin not found, trying static... ");
        // ...then try static
        auto it = available_static_plugins.find(plugin_name.c_str());
        if (it != available_static_plugins.end()) {
          register_func = it->second;
          spdlog::debug("...Found static plugin!");
        }
      }
    }

    // Load plugin!
    if (register_func) {
      try {
        register_func(registrar);
        spdlog::info("...Successfully registered!");
      } catch (const std::exception& e) {
        spdlog::error("...Failed to register plugin:\n\n{}", e.what());
      }
    } else {
      spdlog::error("...Failed to find plugin! :(");
    }
  }
}

FlutterPluginRegisterCallback FlutterView::LoadDynamicPlugin(
    const std::string& plugin_name  //
) {
  // Get registration handle
  // TODO(kerberjg): refactor before merge:
  // - add version handling
  // - load without path (and update LD_LIBRARY_PATH accordingly)
  std::string plugin_path =
      m_config.view.bundle_path + "/lib" + plugin_name + ".so";
  void* lib_handle = dlopen(plugin_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!lib_handle) {
    spdlog::error("Failed to load plugin {}: {}", plugin_name, dlerror());
  } else {
    spdlog::info("Successfully loaded plugin: {}", plugin_name);
  }

  FlutterPluginRegisterCallback register_func;

  PluginGetFuncAddress<FlutterPluginRegisterCallback>(
      lib_handle, "FlutterPluginRegister", &register_func  //
  );

  // Verify and return handle
  if (register_func) {
    return register_func;
    spdlog::info("Successfully registered plugin: {}", plugin_name);
  } else {
    spdlog::error("Failed to find registration function for plugin {}: {}",
                  plugin_name, dlerror()  //
    );

    return nullptr;
  }
}

#endif

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
