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

#include <cassert>
#include <memory>
#include <utility>

#include "app.h"

#if BUILD_BACKEND_HEADLESS_EGL
#include "backend/headless/headless.h"
#elif BUILD_BACKEND_DRM_KMS_EGL
#include "backend/drm_kms_egl/drm_backend.h"
#include "display/drm_display.h"
#elif BUILD_BACKEND_WAYLAND_EGL
#include "backend/wayland_egl/wayland_egl.h"
#elif BUILD_BACKEND_WAYLAND_VULKAN
#include "backend/wayland_vulkan/wayland_vulkan.h"
#endif
#include <key_event_handler.h>
#include <text_input_plugin.h>

#include "configuration/configuration.h"
#include "engine.h"
#include "logging.h"
#ifdef ENABLE_PLUGIN_GSTREAMER_EGL
#include "plugins/gstreamer_egl/gstreamer_egl.h"
#endif
#ifdef ENABLE_PLUGIN_COMP_SURF
#include "compositor_surface.h"
#endif

#if ENABLE_PLUGINS
extern void PluginsApiRegisterPlugins(FlutterDesktopEngineRef engine);
#endif

#if !BUILD_BACKEND_DRM_KMS_EGL
#include "wayland/display.h"
#include "wayland/window.h"
#endif

extern void SetUpCommonEngineState(FlutterDesktopEngineState* state,
                                   FlutterView* view);

FlutterView::FlutterView(Configuration::Config config,
                         const size_t index,
                         const std::shared_ptr<IDisplay>& display)
    : m_display(display), m_config(std::move(config)), m_index(index) {
#if BUILD_BACKEND_HEADLESS_EGL
  m_backend = std::make_shared<HeadlessBackend>(
      m_config.view.width.value_or(kDefaultViewWidth),
      m_config.view.height.value_or(kDefaultViewHeight),
      m_config.debug_backend.value_or(false), kEglBufferSize);
#elif BUILD_BACKEND_DRM_KMS_EGL
  {
    auto parse_tri = [](const std::optional<std::string>& s,
                        drm_config::TriState def =
                            drm_config::TriState::kAuto) {
      if (!s.has_value() || s->empty() || *s == "auto") {
        return def;
      }
      if (*s == "yes" || *s == "true" || *s == "on") {
        return drm_config::TriState::kYes;
      }
      if (*s == "no" || *s == "false" || *s == "off") {
        return drm_config::TriState::kNo;
      }
      spdlog::warn("[FlutterView] drm tri-state '{}' unrecognized; using auto",
                   *s);
      return drm_config::TriState::kAuto;
    };
    auto parse_compositor = [](const std::optional<std::string>& s) {
      if (!s.has_value() || s->empty() || *s == "auto") {
        return drm_config::Compositor::kAuto;
      }
      if (*s == "planes") {
        return drm_config::Compositor::kPlanes;
      }
      if (*s == "gl") {
        return drm_config::Compositor::kGl;
      }
      spdlog::warn("[FlutterView] drm-compositor '{}' unrecognized; using auto",
                   *s);
      return drm_config::Compositor::kAuto;
    };
    auto parse_modeset = [](const std::optional<std::string>& s) {
      if (!s.has_value() || s->empty() || *s == "auto") {
        return drm_config::Modeset::kAuto;
      }
      if (*s == "legacy") {
        return drm_config::Modeset::kLegacy;
      }
      if (*s == "atomic") {
        return drm_config::Modeset::kAtomic;
      }
      spdlog::warn("[FlutterView] drm-modeset '{}' unrecognized; using auto",
                   *s);
      return drm_config::Modeset::kAuto;
    };
    auto parse_format = [](const std::optional<std::string>& s) {
      if (!s.has_value() || s->empty() || *s == "auto") {
        return drm_config::PrimaryFormat::kAuto;
      }
      if (*s == "xrgb8888") {
        return drm_config::PrimaryFormat::kXrgb8888;
      }
      if (*s == "xbgr8888") {
        return drm_config::PrimaryFormat::kXbgr8888;
      }
      if (*s == "argb8888") {
        return drm_config::PrimaryFormat::kArgb8888;
      }
      if (*s == "abgr8888") {
        return drm_config::PrimaryFormat::kAbgr8888;
      }
      if (*s == "rgb565") {
        return drm_config::PrimaryFormat::kRgb565;
      }
      spdlog::warn(
          "[FlutterView] drm-primary-format '{}' unrecognized "
          "(expected auto|xrgb8888|xbgr8888|argb8888|abgr8888|rgb565); "
          "using auto",
          *s);
      return drm_config::PrimaryFormat::kAuto;
    };

    // --fullscreen on DRM = drive the panel at its preferred mode with no
    // framing. Clearing width/height here lets DrmBackend fall through to
    // cfg_.width.value_or(mode_.hdisplay), i.e. the native mode. When -f
    // is combined with explicit width/height, -f wins — matches how most
    // CLI tools handle a scalar "use native" flag vs. explicit sizing.
    const bool fullscreen = m_config.view.fullscreen.value_or(false);
    DrmConfig cfg{
        m_config.view.drm_device.value_or("/dev/dri/card1"),
        fullscreen ? std::optional<uint32_t>{} : m_config.view.width,
        fullscreen ? std::optional<uint32_t>{} : m_config.view.height,
        m_config.debug_backend.value_or(false),
    };
    // Treat empty TOML/env/CLI string as "unset" — operator-friendly:
    // `drm_connector = ""` in TOML shouldn't force a strict empty-name
    // match against zero connectors.
    if (m_config.view.drm_connector.has_value() &&
        !m_config.view.drm_connector->empty()) {
      cfg.connector_name = m_config.view.drm_connector;
    }
    cfg.compositor = parse_compositor(m_config.view.drm_compositor);
    cfg.modeset = parse_modeset(m_config.view.drm_modeset);
    cfg.allow_nonblock_modeset =
        parse_tri(m_config.view.drm_allow_nonblock_modeset);
    cfg.primary_format = parse_format(m_config.view.drm_primary_format);
    cfg.overlay_planes = parse_tri(m_config.view.drm_overlay_planes);
    cfg.explicit_sync = parse_tri(m_config.view.drm_explicit_sync);
    cfg.async_flip = parse_tri(m_config.view.drm_async_flip);

    // DrmDisplay (constructed by App::MakeDisplay, before us) owns the
    // process-wide libseat session. Pull the raw pointer — may be null
    // when no seat backend is available, in which case DrmBackend takes
    // the legacy direct-open path. In a DRM build, MakeDisplay always
    // returns a DrmDisplay, so dynamic_cast fails only on programmer
    // error; assert keeps the invariant loud.
    auto* drm_display = dynamic_cast<DrmDisplay*>(m_display.get());
    assert(drm_display != nullptr);
    m_backend = DrmBackend::Create(cfg, drm_display->session());

    // DrmBackend::Create returns nullptr on any init failure (libseat
    // take_device, drmSetMaster, no usable connector, GBM/EGL setup,
    // …). Continuing would dereference a null backend in Engine::Run
    // and SEGV; fail-fast with the same exit path as a missing bundle.
    if (!m_backend) {
      spdlog::critical("[FlutterView] DRM backend init failed; aborting");
      exit(EXIT_FAILURE);
    }

    // Wire the HW cursor (if Create succeeded in opening one) to the
    // seat dispatch thread so pointer events move the on-screen sprite.
    // The pointer stays valid for the FlutterView's lifetime — both
    // m_backend and drm_display are members destroyed in declaration
    // order during ~FlutterView.
    if (auto* drm_backend = dynamic_cast<DrmBackend*>(m_backend.get());
        drm_backend != nullptr) {
      drm_display->SetCursor(drm_backend->drm_cursor());
    }
  }
#elif BUILD_BACKEND_WAYLAND_EGL
  {
    auto* wl = dynamic_cast<Display*>(display.get());
    m_backend = std::make_shared<WaylandEglBackend>(
        wl->GetDisplay(), m_config.view.width.value_or(kDefaultViewWidth),
        m_config.view.height.value_or(kDefaultViewHeight),
        m_config.debug_backend.value_or(false), kEglBufferSize);
  }
#elif BUILD_BACKEND_WAYLAND_VULKAN
  {
    auto* wl = dynamic_cast<Display*>(display.get());
    m_backend = std::make_shared<WaylandVulkanBackend>(
        wl->GetDisplay(), m_config.view.width.value_or(kDefaultViewWidth),
        m_config.view.height.value_or(kDefaultViewHeight),
        m_config.debug_backend.value_or(false));
  }
#endif

  SPDLOG_DEBUG("Width: {}, Height: {}",
               m_config.view.width.value_or(kDefaultViewWidth),
               m_config.view.height.value_or(kDefaultViewWidth));

#if !BUILD_BACKEND_DRM_KMS_EGL
  auto* wl = dynamic_cast<Display*>(display.get());
  m_wayland_window = std::make_shared<WaylandWindow>(
      m_index, std::dynamic_pointer_cast<Display>(display),
      m_config.view.window_type,
      wl->GetWlOutput(m_config.view.wl_output_index.value_or(0)),
      m_config.view.wl_output_index.value_or(0), m_config.app_id,
      m_config.view.fullscreen.value_or(false),
      m_config.view.width.value_or(kDefaultViewWidth),
      m_config.view.height.value_or(kDefaultViewWidth),
      m_config.view.pixel_ratio.value_or(kDefaultPixelRatio),
      m_config.view.activation_area_x, m_config.view.activation_area_y,
      m_config.view.activation_area_width, m_config.view.activation_area_height,
      m_backend.get(), m_config.view.ivi_surface_id.value_or(0));
#endif

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
  m_display->SetViewControllerState(m_state->engine_state->view_controller);

#if ENABLE_PLUGINS
  PluginsApiRegisterPlugins(m_state->engine_state.get());
#endif
}

FlutterView::~FlutterView() = default;

#if !BUILD_BACKEND_DRM_KMS_EGL
Display* FlutterView::GetDisplay() const {
  return dynamic_cast<Display*>(m_display.get());
}
#endif

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

#if BUILD_BACKEND_DRM_KMS_EGL
  // Hand the engine handle + platform task runner to the DRM backend
  // so OnSessionResumed can call ScheduleFrame after a VT round-trip
  // and PostOnVsync can marshal OnVsync onto the FlutterEngineRun
  // thread (Flutter rejects OnVsync from any other thread with
  // kInternalInconsistency).
  if (auto* drm_backend = dynamic_cast<DrmBackend*>(m_backend.get());
      drm_backend != nullptr) {
    drm_backend->SetEngineHandle(m_flutter_engine->GetFlutterEngine());
    drm_backend->SetPlatformTaskRunner(
        m_flutter_engine->GetPlatformTaskRunner());
  }
#endif

  // notify display update
  FlutterEngineDisplay display{};
  display.struct_size = sizeof(FlutterEngineDisplay);
  display.display_id = 1;
  display.single_display = true;
  display.refresh_rate =
      m_display->GetRefreshRate(static_cast<uint32_t>(m_index));
#if BUILD_BACKEND_DRM_KMS_EGL
  // DrmDisplay is constructed from the config-level width/height in app.cc,
  // which may still hold the TOML-specified size even when `-f` cleared those
  // values for the backend. Query the backend for the resolved FB size so
  // fullscreen actually gets mode dims here instead of a stale 1024x768 etc.
  const auto width = static_cast<int32_t>(m_backend->width());
  const auto height = static_cast<int32_t>(m_backend->height());
#else
  auto [width, height] = m_wayland_window->GetSize();
#endif
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

#if BUILD_BACKEND_DRM_KMS_EGL
  // On the DRM path there is no WaylandWindow to trigger the initial
  // window-metrics event. Without it Flutter never learns the viewport
  // size and never schedules a frame. Send it explicitly now that the
  // engine is running.
  {
    const auto result = m_flutter_engine->SetWindowSize(height, width);
    spdlog::info("[DrmBackend] SendWindowMetrics {}x{} result={}", width,
                 height, static_cast<int>(result));
  }
#else
  // Engine events are decoded by surface pointer
  dynamic_cast<Display*>(m_display.get())
      ->SetEngine(m_wayland_window->GetBaseSurface(), m_flutter_engine.get());
  m_wayland_window->SetEngine(m_flutter_engine);
#endif

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
      index, std::dynamic_pointer_cast<Display>(m_display), m_wayland_window,
      h_module, assets_path, cache_folder, misc_folder, type, z_order, sync,
      width, height, x, y);

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
  const auto compositor =
      dynamic_cast<Display*>(m_display.get())->GetCompositor();
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

#if BUILD_COMPOSITOR
void FlutterView::RegisterCompositorSurface(
    FlutterPlatformViewIdentifier id,
    std::shared_ptr<ICompositorSurface> surface) {
  if (!m_backend) {
    return;
  }
  if (surface) {
    m_backend->RegisterCompositorSurface(id, std::move(surface));
  } else {
    m_backend->UnregisterCompositorSurface(id);
  }
}

void FlutterView::UnregisterCompositorSurface(
    FlutterPlatformViewIdentifier id) {
  if (!m_backend) {
    return;
  }
  m_backend->UnregisterCompositorSurface(id);
}

void FlutterView::ResizeCompositorSurface(FlutterPlatformViewIdentifier id,
                                          int32_t width,
                                          int32_t height) {
  if (!m_backend) {
    return;
  }
  m_backend->ResizeCompositorSurface(id, width, height);
}
#endif
