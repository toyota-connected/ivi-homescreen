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
#include <filesystem>
#include "logging/logging.h"

#include <cassert>
#include <memory>
#include <utility>

#include "app.h"

#if BUILD_BACKEND_HEADLESS_EGL
#include "backend/headless/headless.h"
#elif BUILD_BACKEND_DRM_KMS_EGL
#include "backend/drm_kms_egl/drm_backend.h"
#include "display/drm_display.h"
#elif BUILD_BACKEND_SOFTWARE
#include "backend/software/sink_factory.h"
#include "backend/software/software_backend.h"
#elif BUILD_BACKEND_WAYLAND_EGL
#include "backend/wayland_egl/wayland_egl.h"
#elif BUILD_BACKEND_WAYLAND_VULKAN
#include "backend/wayland_vulkan/wayland_vulkan.h"
#else
#error "no Flutter backend selected (see flutter_view.h)"
#endif
#include <key_event_handler.h>
#include <text_input_plugin.h>

#include "configuration/configuration.h"
#include "engine.h"
#include "logging.h"
#include "platform/homescreen/flutter_desktop_messenger.h"
#ifdef ENABLE_PLUGIN_GSTREAMER_EGL
#include "plugins/gstreamer_egl/gstreamer_egl.h"
#endif
#ifdef ENABLE_PLUGIN_COMP_SURF
#include "compositor_surface.h"
#endif

#if ENABLE_PLUGINS
extern void PluginsApiRegisterPlugins(FlutterDesktopEngineRef engine);
#endif

#if !BUILD_BACKEND_DRM_KMS_EGL && !BUILD_BACKEND_SOFTWARE
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN || \
    BUILD_BACKEND_HEADLESS_EGL
#include "wayland/display.h"
#include "wayland/window.h"
#endif
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
    if (m_config.view.drm_mode.has_value() &&
        !m_config.view.drm_mode->empty()) {
      cfg.mode_spec = m_config.view.drm_mode;
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
      // Push the resolved framebuffer dimensions into the seat's
      // cursor-clamp viewport. App::MakeDisplay seeds DrmDisplay with
      // the config's view.width/height (defaults 1024x768) before the
      // backend has resolved the actual mode — `-f` then promotes the
      // FB to the full mode dimensions, leaving the seat's pointer
      // clamp stuck at the config size unless we update it here.
      drm_display->SetViewportSize(static_cast<int32_t>(drm_backend->width()),
                                   static_cast<int32_t>(drm_backend->height()));
      drm_display->SetCursor(drm_backend->drm_cursor());
    }
  }
#elif BUILD_BACKEND_WAYLAND_EGL
  {
    auto* wl = dynamic_cast<Display*>(display.get());
    m_backend = std::make_shared<WaylandEglBackend>(
        wl, wl->GetDisplay(), m_config.view.width.value_or(kDefaultViewWidth),
        m_config.view.height.value_or(kDefaultViewHeight),
        m_config.debug_backend.value_or(false), kEglBufferSize);
  }
#elif BUILD_BACKEND_WAYLAND_VULKAN
  {
    auto* wl = dynamic_cast<Display*>(display.get());
    // Mesa's Vulkan WSI on Wayland needs zwp_linux_dmabuf_v1 (modern) or
    // wl_drm (legacy) to allocate GPU buffers. Without one of these,
    // vkGetPhysicalDeviceSurfaceFormatsKHR returns SURFACE_LOST on the
    // first surface call and the swapchain init bails. Warn loudly here so
    // the user has a one-line root cause instead of a deep VK abort.
    if (!wl->HasLinuxDmabuf() && !wl->HasWlDrm()) {
      spdlog::warn(
          "[WaylandVulkanBackend] compositor advertises neither "
          "zwp_linux_dmabuf_v1 nor wl_drm — Mesa Vulkan WSI cannot "
          "allocate swapchain images. Swapchain init will fail; fix on the "
          "compositor side (e.g. Weston needs --backend=drm-backend or its "
          "linux-dmabuf support built in).");
    }
    m_backend = std::make_shared<WaylandVulkanBackend>(
        wl, wl->GetDisplay(), m_config.view.width.value_or(kDefaultViewWidth),
        m_config.view.height.value_or(kDefaultViewHeight),
        m_config.debug_backend.value_or(false));
  }
#elif BUILD_BACKEND_SOFTWARE
  {
    // Sink is picked at startup from IVI_SW_SINK. Default 'none' just
    // discards frames; 'memory' keeps the latest in-process; 'file:
    // <pattern>' writes PAM-format snapshots to disk.
    m_backend = std::make_shared<SoftwareBackend>(
        m_config.view.width.value_or(kDefaultViewWidth),
        m_config.view.height.value_or(kDefaultViewHeight), MakeSinkFromEnv());
  }
#endif

  SPDLOG_DEBUG("Width: {}, Height: {}",
               m_config.view.width.value_or(kDefaultViewWidth),
               m_config.view.height.value_or(kDefaultViewWidth));

#if !BUILD_BACKEND_DRM_KMS_EGL && !BUILD_BACKEND_SOFTWARE
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

  // Create engine_state locally so we own it through configuration; it'll
  // be moved into m_flutter_engine in Initialize() so that it survives
  // FlutterEngineDeinitialize. m_state holds a non-owning raw pointer.
  m_pending_engine_state = std::make_unique<FlutterDesktopEngineState>();
  m_state->engine_state = m_pending_engine_state.get();
  m_state->engine_state->view_controller = m_state.get();

  // Set the flutter assets folder
  std::filesystem::path path = m_config.view.bundle_path;
  path /= kBundleFlutterAssets;
  m_state->engine_state->flutter_asset_directory = path.generic_string();

  SetUpCommonEngineState(m_state->engine_state, this);

  // Set up the keyboard handlers
  auto internal_plugin_messenger =
      m_state->engine_state->internal_plugin_registrar->messenger();
  m_state->keyboard_hook_handlers.push_back(
      std::make_unique<flutter::KeyEventHandler>(internal_plugin_messenger));
  m_state->keyboard_hook_handlers.push_back(
      std::make_unique<flutter::TextInputPlugin>(internal_plugin_messenger));
  m_display->SetViewControllerState(m_state->engine_state->view_controller);

#if ENABLE_PLUGINS
  PluginsApiRegisterPlugins(m_state->engine_state);
#endif
}

FlutterView::~FlutterView() {
  // Latch shutting_down on the texture registrar BEFORE m_state destructs.
  // Plugin streaming threads (video_player's gstreamer handoff is the
  // canonical case) call FlutterDesktopTexture{Make,Clear}Current and
  // MarkExternalTextureFrameAvailable from their own threads, independent
  // of the Flutter engine's lifecycle. Once m_state.reset() runs, the
  // texture_registrar inside m_state->engine_state is freed and the
  // engine/view_controller back-pointers dangle. Setting the gate here
  // makes those entry points return false instead of dereferencing freed
  // memory while the plugin's pipeline winds down.
  if (m_state && m_state->engine_state &&
      m_state->engine_state->texture_registrar) {
    m_state->engine_state->texture_registrar->shutting_down.store(
        true, std::memory_order_release);
  }

  // Null the messenger's engine pointer for the same reason — the
  // singleton plugin_common_glib::MainLoop outlives main() and its glib
  // thread can dispatch a queued gst bus message into a freed
  // VideoPlayer, which calls FlutterDesktopMessengerSend on a messenger
  // whose engine_state has been freed. SetEngine takes the messenger's
  // own mutex; FlutterDesktopMessengerSendWithReply takes the same
  // mutex and bails when engine is null.
  if (m_state && m_state->engine_state && m_state->engine_state->messenger) {
    m_state->engine_state->messenger->SetEngine(nullptr);
  }

  // Tear down any backend-owned vsync/event-loop monitor before
  // m_flutter_engine destructs. DRM's monitor is an asio async_wait on
  // the drm fd living on the engine's platform task runner; if it's
  // still outstanding when Engine::~Engine resets the runner,
  // TaskRunner::~TaskRunner blocks forever joining a worker parked in
  // epoll_wait waiting for an event that will never arrive. The default
  // virtual is a no-op for backends without a monitor.
  if (m_backend) {
    m_backend->StopVsyncMonitor();
  }
}

#if !BUILD_BACKEND_DRM_KMS_EGL && !BUILD_BACKEND_SOFTWARE
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

  // Hand engine_state ownership to Engine so it survives
  // FlutterEngineDeinitialize. m_state->engine_state stays as a raw
  // pointer — same target, different owner.
  m_flutter_engine->TakeEngineState(std::move(m_pending_engine_state));

  m_flutter_engine->Run(m_state->engine_state);

  if (!m_flutter_engine->IsRunning()) {
    spdlog::critical("Failed to Run Engine");
    exit(EXIT_FAILURE);
  }

  // Hand the engine handle + platform task runner to the backend so
  // post-Engine::Run lifecycle hooks (DRM's OnSessionResumed →
  // ScheduleFrame; WaylandEgl's wp_presentation_feedback dispatch) can
  // marshal back to the FlutterEngineRun thread without a dynamic_cast.
  // Backends that don't need either inherit no-op defaults from Backend.
  if (m_backend) {
    m_backend->SetEngineHandle(m_flutter_engine->GetFlutterEngine());
    m_backend->SetPlatformTaskRunner(m_flutter_engine->GetPlatformTaskRunner());
  }

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
#elif BUILD_BACKEND_SOFTWARE
  // No WaylandWindow + no DRM backend size source — use the config dims
  // directly.
  const auto width =
      static_cast<int32_t>(m_config.view.width.value_or(kDefaultViewWidth));
  const auto height =
      static_cast<int32_t>(m_config.view.height.value_or(kDefaultViewHeight));
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
    const auto result = m_flutter_engine->SetWindowSize(
        static_cast<size_t>(height), static_cast<size_t>(width));
    spdlog::info("[DrmBackend] SendWindowMetrics {}x{} result={}", width,
                 height, static_cast<int>(result));
  }
#elif BUILD_BACKEND_SOFTWARE
  // Same reason as DRM: no WaylandWindow to trigger the initial
  // window-metrics event. Send it explicitly here so the bundle's Dart
  // side gets a non-zero viewport on its first frame.
  {
    const auto result = m_flutter_engine->SetWindowSize(
        static_cast<size_t>(height), static_cast<size_t>(width));
    spdlog::info("[SoftwareBackend] SendWindowMetrics {}x{} result={}", width,
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
