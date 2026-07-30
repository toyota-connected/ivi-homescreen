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
#include <algorithm>
#include <chrono>
#include <filesystem>
#include "logging/logging.h"

#include <cassert>
#include <cmath>
#include <memory>
#include <utility>

#include "app.h"
#include "backend/backend_registry.h"
#include "backend/register_backends.h"

// Independent guards so several backends can be compiled into one binary
// (Backend::Create selects per view at runtime). Header guards make the
// repeated drm_display.h include harmless.
#if BUILD_BACKEND_DRM_KMS_EGL
#include "backend/drm_kms_egl/drm_backend.h"
#include "backend/drm_kms_egl/drm_cursor.h"  // DrmCursor -> ICursorShapeSink*
#include "backend/drm_kms_egl/gl_cursor.h"   // GlCursor -> ICursorShapeSink*
#include "display/drm_display.h"
#endif
#if BUILD_BACKEND_DRM_KMS_VULKAN
#include "backend/drm_kms_egl/drm_cursor.h"  // DrmCursor -> ICursorShapeSink*
#include "backend/drm_kms_vulkan/vulkan_drm_backend.h"
#include "display/drm_display.h"
#endif
#if BUILD_BACKEND_SOFTWARE
#include "backend/software/sink_factory.h"
#include "backend/software/software_backend.h"
#include "display/software_display.h"
#endif
#if BUILD_BACKEND_HEADLESS_EGL
#include "backend/headless_egl/headless_egl.h"
#endif
#if BUILD_BACKEND_WAYLAND_EGL
#include "backend/wayland_egl/wayland_egl.h"
#endif
#if BUILD_BACKEND_WAYLAND_VULKAN
#include "backend/wayland_vulkan/wayland_vulkan.h"
#endif
#include <key_event_handler.h>
#include <text_input_plugin.h>

#include "configuration/configuration.h"
#include "display/output.h"          // homescreen::BackendFamily
#include "display/output_manager.h"  // homescreen::OutputManager
#include "engine.h"
#include "logging.h"
#include "main_loop_waker.h"
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

#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
#include "wayland/display.h"
#include "wayland/window.h"
#endif

extern void SetUpCommonEngineState(FlutterDesktopEngineState* state,
                                   FlutterView* view);

#if BUILD_HUD
#include "backend/hud/ihud.h"

namespace {
// Translate the view's [hud] config into an ihs::hud::HudConfig, returning
// whether config asked for the HUD (OR-ed with IVI_HUD in the backend).
bool BuildHudConfig(const Configuration::Config& config,
                    ihs::hud::HudConfig& out) {
  const auto& h = config.hud;
  if (h.corner) {
    const std::string& c = *h.corner;
    if (c == "top-right") {
      out.corner = ihs::hud::HudConfig::Corner::kTopRight;
    } else if (c == "bottom-left") {
      out.corner = ihs::hud::HudConfig::Corner::kBottomLeft;
    } else if (c == "bottom-right") {
      out.corner = ihs::hud::HudConfig::Corner::kBottomRight;
    } else {
      out.corner = ihs::hud::HudConfig::Corner::kTopLeft;
    }
  }
  if (h.margin) {
    out.margin = static_cast<float>(*h.margin);
  }
  if (h.font_scale) {
    out.font_scale = static_cast<float>(*h.font_scale);
  }
  if (h.bg_alpha) {
    out.bg_alpha = static_cast<float>(*h.bg_alpha);
  }
  // "#RRGGBB" or "#RRGGBBAA" -> normalized floats.
  if (h.text_color && !h.text_color->empty()) {
    std::string s = *h.text_color;
    if (s.front() == '#') {
      s.erase(0, 1);
    }
    auto hex = [&](size_t i) -> float {
      const auto b =
          static_cast<unsigned>(std::stoul(s.substr(i, 2), nullptr, 16));
      return static_cast<float>(b) / 255.0f;
    };
    try {
      if (s.size() >= 6) {
        out.text_r = hex(0);
        out.text_g = hex(2);
        out.text_b = hex(4);
        out.text_a = s.size() >= 8 ? hex(6) : 1.0f;
      }
    } catch (const std::exception&) {
      // Malformed color: keep the default (opaque white).
    }
  }
  return h.enable.value_or(false);
}
}  // namespace
#endif  // BUILD_HUD

// The single per-view Backend factory (declared in backend/backend.h).
// Delegates to the runtime BackendRegistry: the resolved descriptor's
// make_backend holds the concrete-backend construction + post-creation wiring
// that used to live here as an #if/#elif chain (now in
// backend/register_backends.cc). The backend is resolved per view from its own
// config (ResolveKeyForConfig), so each view runs the renderer its bundle asked
// for -- matching the device-context display App placed it on.
std::shared_ptr<Backend> Backend::Create(
    const Configuration::Config& config,
    const std::shared_ptr<IDisplay>& display) {
  auto& registry = backend::BackendRegistry::Instance();
  const std::string key = ResolveKeyForConfig(registry, config);
  const backend::BackendDescriptor* descriptor = registry.Resolve(key);
  if (descriptor == nullptr) {
    ihs::log::critical("[FlutterView] no backend resolved for view (key='{}')",
                       key);
    return nullptr;
  }
  auto backend = descriptor->make_backend(config, display.get());
#if BUILD_HUD
  if (backend) {
    ihs::hud::HudConfig hc;
    const bool enable = BuildHudConfig(config, hc);
    backend->SetHudConfig(hc, enable);
  }
#endif
  return backend;
}

FlutterView::FlutterView(Configuration::Config config,
                         const size_t index,
                         const std::shared_ptr<IDisplay>& display)
    : m_display(display), m_config(std::move(config)), m_index(index) {
  m_backend = Backend::Create(m_config, display);

  IHS_DEBUG("Width: {}, Height: {}",
            m_config.view.width.value_or(kDefaultViewWidth),
            m_config.view.height.value_or(kDefaultViewHeight));

#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
  // Construct the WaylandWindow only when the active backend drives a Wayland
  // Display. In a multi-backend binary the DRM/software backends are compiled
  // in too; their IDisplay is not a wayland Display, so the cast is null and we
  // skip (those backends render without a WaylandWindow).
  if (auto* wl = dynamic_cast<Display*>(display.get())) {
    // A [[view.output]] array binds one engine to several outputs, but the
    // one-engine -> N-outputs present router is drm-kms-egl only (the Wayland
    // backend has no per-output surface / present_view path). Drive the primary
    // output here and say so, rather than dropping the rest silently: use one
    // view per output (a bundle each) on Wayland, or the DRM backend for a
    // single engine spanning outputs.
    if (!m_config.view.additional_outputs.empty()) {
      ihs::log::warn(
          "[FlutterView] additional [[view.output]] entries are ignored: the "
          "Wayland backend drives one output per view. Use one view/bundle per "
          "output, or the drm-kms-egl backend for one engine across outputs.");
    }
    // Pin the view to a physical output by name when [view.output] names one:
    // OutputManager applies the view's OutputMatch through the Display's
    // IOutputProvider (the same resolver the DRM path uses for connectors) and
    // returns the wl_output name, which maps back to the index a WaylandWindow
    // targets. Fall back to the explicit wl_output_index when no [view.output]
    // constraint is set or the named output is not live -- preserving
    // single-output behavior.
    uint32_t wl_output_index = m_config.view.wl_output_index.value_or(0);
    if (const auto resolved = homescreen::OutputManager::ResolveForView(
            m_config, display.get(), homescreen::BackendFamily::kWayland)) {
      if (const auto idx = wl->WlOutputIndexForName(*resolved)) {
        wl_output_index = *idx;
      } else {
        ihs::log::warn(
            "[FlutterView] resolved wl_output '{}' is not live; using "
            "wl_output_index {}",
            *resolved, wl_output_index);
      }
    }
    m_wayland_window = std::make_shared<WaylandWindow>(
        m_index, std::dynamic_pointer_cast<Display>(display),
        m_config.view.window_type, wl->GetWlOutput(wl_output_index),
        wl_output_index, m_config.app_id,
        m_config.view.fullscreen.value_or(false),
        m_config.view.width.value_or(kDefaultViewWidth),
        m_config.view.height.value_or(kDefaultViewHeight),
        m_config.view.pixel_ratio.value_or(kDefaultPixelRatio),
        m_config.view.activation_area_x, m_config.view.activation_area_y,
        m_config.view.activation_area_width,
        m_config.view.activation_area_height, m_backend.get(),
        m_config.view.ivi_surface_id.value_or(0), this);
  }
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
  // TextInputPlugin is owned by the controller state so that KeyEventHandler
  // can hold a raw pointer to it for fallback text insertion.
  m_state->text_input_plugin =
      std::make_unique<flutter::TextInputPlugin>(internal_plugin_messenger);

#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
  // Wire the compositor IME (zwp_text_input_v3) seam: the plugin drives the
  // Wayland Display's text-input on show/hide/cursor-rect, and Display posts
  // the IME's commit/preedit edits back onto the engine strand. A no-op when
  // the compositor never advertised the text-input manager.
  if (auto* wl_display = dynamic_cast<Display*>(m_display.get())) {
    auto* plugin = m_state->text_input_plugin.get();
    plugin->SetImeActivate([wl_display](uint32_t hint, uint32_t purpose,
                                        int32_t x, int32_t y, int32_t w,
                                        int32_t h) {
      wl_display->ActivateTextInput(hint, purpose, x, y, w, h);
    });
    plugin->SetImeDeactivate(
        [wl_display]() { wl_display->DeactivateTextInput(); });
    plugin->SetImeUpdateCursorRect(
        [wl_display](int32_t x, int32_t y, int32_t w, int32_t h) {
          wl_display->UpdateTextInputCursorRect(x, y, w, h);
        });
    plugin->SetImeUpdateSurrounding([wl_display](const std::string& utf8,
                                                 uint32_t cursor,
                                                 uint32_t anchor) {
      wl_display->UpdateTextInputSurrounding(utf8, cursor, anchor);
    });
  }
#endif

  // Create KeyEventHandler and register it.
  auto keh_owned =
      std::make_unique<flutter::KeyEventHandler>(internal_plugin_messenger);
  m_key_event_handler = keh_owned.get();
  m_state->keyboard_hook_handlers.push_back(std::move(keh_owned));

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

  // Ordered shutdown. The three steps below must run in this order; relying
  // on member-destruction order instead races the engine's rasterizer thread
  // against GL/WSI teardown (SIGSEGV) or strands GPU resources behind a
  // freed wl_surface.
  //
  // 1. Stop the backend's vsync/event-loop monitor while the engine and its
  //    platform task runner are still alive. DRM's monitor is an asio
  //    async_wait on the drm fd living on that runner; if it's still
  //    outstanding when the runner is destroyed, TaskRunner::~TaskRunner
  //    blocks forever joining a worker parked in epoll_wait. This does NOT
  //    touch GL/WSI resources. No-op for backends without a monitor.
  if (m_backend) {
    m_backend->StopVsyncMonitor();
  }

  // 2. Point of control: stop the engine and join the platform, UI and
  //    rasterizer threads. Until this returns the rasterizer can still call
  //    into the backend (VsyncTrampoline / PresentLayers) and hold a GL
  //    context current. Doing this explicitly here — rather than leaving it
  //    to m_flutter_engine's destruction further down — guarantees no engine
  //    thread is alive for step 3.
  if (m_flutter_engine) {
    m_flutter_engine->Shutdown();
  }

  // 3. Now that no engine thread is alive, release the backend's GL contexts
  //    and render surfaces, while m_wayland_window (wl_surface) and m_display
  //    (wl_display) are still alive — both are destroyed later, in member
  //    order — so the EGL surface / EGLDisplay are still valid. No-op for
  //    backends that don't hold such resources.
  if (m_backend) {
    m_backend->ReleaseRenderSurfaces();
  }
}

#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
Display* FlutterView::GetDisplay() const {
  return dynamic_cast<Display*>(m_display.get());
}
#endif

void FlutterView::Initialize() {
  // Engine / Dart VM switches -> command_line_argv. argv[0] is the app id.
  std::vector<const char*> m_command_line_args_c;
  m_command_line_args_c.reserve(m_config.view.engine_args.size() + 2);
  m_command_line_args_c.push_back(m_config.app_id.c_str());
  for (const auto& arg : m_config.view.engine_args) {
    m_command_line_args_c.push_back(arg.c_str());
  }

#if BUILD_BACKEND_HEADLESS_EGL
  // The headless-EGL encode backend requires Impeller. Under the Skia GL
  // renderer a startup race in the raster cache intermittently drops static
  // layers -- e.g. a whole-run black background on ~half of cold starts --
  // because a cached layer image is captured empty. Impeller GLES has no such
  // cache and paints the full layer tree every frame, which is also what an
  // encoder wants. Default it on for this backend unless the deployment pinned
  // the flag either way (e.g.
  // --enable-impeller=false to opt back into Skia). The literal has static
  // storage, so its pointer stays valid for the synchronous Engine
  // construction.
  static constexpr char kImpellerSwitch[] = "--enable-impeller";
  if (dynamic_cast<HeadlessEglBackend*>(m_backend.get()) != nullptr) {
    const auto& args = m_config.view.engine_args;
    const bool pinned =
        std::any_of(args.begin(), args.end(), [](const std::string& a) {
          return a == kImpellerSwitch || a.rfind("--enable-impeller=", 0) == 0;
        });
    if (!pinned) {
      m_command_line_args_c.push_back(kImpellerSwitch);
      ihs::log::info(
          "[FlutterView] headless-egl: enabling Impeller (Skia GL raster cache "
          "intermittently drops static layers on this path)");
    }
  }
#endif

  // Arguments to the Dart entrypoint main(List<String>) -> dart_entrypoint_argv
  // (no argv[0] convention).
  std::vector<const char*> m_dart_entrypoint_args_c;
  m_dart_entrypoint_args_c.reserve(m_config.view.dart_args.size());
  for (const auto& arg : m_config.view.dart_args) {
    m_dart_entrypoint_args_c.push_back(arg.c_str());
  }

#if BUILD_ACCESSIBILITY
  m_accessibility_tree = std::make_shared<AccessibilityTree>();
  m_state->engine_state->accessibility_tree = m_accessibility_tree.get();
#endif

  m_flutter_engine = std::make_shared<Engine>(
      this, m_index, m_command_line_args_c, m_dart_entrypoint_args_c,
      m_config.view.bundle_path,
      m_config.view.accessibility_features.value_or(0),
      m_config.view.merge_render_platform.value_or(false));

  m_state->engine = m_flutter_engine.get();

  // Hand engine_state ownership to Engine so it survives
  // FlutterEngineDeinitialize. m_state->engine_state stays as a raw
  // pointer — same target, different owner.
  m_flutter_engine->TakeEngineState(std::move(m_pending_engine_state));

  m_flutter_engine->Run(m_state->engine_state);

  if (!m_flutter_engine->IsRunning()) {
    ihs::log::critical("Failed to Run Engine");
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
  // Resolve the engine's render extent from whichever backend is active at
  // RUNTIME. Each compiled backend gets a chance to report its resolved size
  // (DRM/software adopt the panel's native mode so fullscreen gets mode dims;
  // Wayland uses the window size); fall back to the configured dims. Runtime
  // dispatch so any combination of backends compiled into one binary works.
  int32_t width = 0;
  int32_t height = 0;
  bool size_resolved = false;
#if BUILD_BACKEND_DRM_KMS_EGL
  if (auto* drm = dynamic_cast<DrmBackend*>(m_backend.get())) {
    width = static_cast<int32_t>(drm->width());
    height = static_cast<int32_t>(drm->height());
    size_resolved = true;
  }
#endif
#if BUILD_BACKEND_DRM_KMS_VULKAN
  if (!size_resolved) {
    if (auto* drm = dynamic_cast<VulkanDrmBackend*>(m_backend.get())) {
      width = static_cast<int32_t>(drm->width());
      height = static_cast<int32_t>(drm->height());
      size_resolved = true;
    }
  }
#endif
#if BUILD_BACKEND_SOFTWARE
  if (!size_resolved) {
    if (auto* sw_backend = dynamic_cast<SoftwareBackend*>(m_backend.get())) {
      width = static_cast<int32_t>(sw_backend->width());
      height = static_cast<int32_t>(sw_backend->height());
      size_resolved = true;
    }
  }
#endif
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
  if (!size_resolved && m_wayland_window) {
    const auto sz = m_wayland_window->GetSize();
    width = static_cast<int32_t>(sz.first);
    height = static_cast<int32_t>(sz.second);
    size_resolved = true;
  }
#endif
  if (!size_resolved) {
    width =
        static_cast<int32_t>(m_config.view.width.value_or(kDefaultViewWidth));
    height =
        static_cast<int32_t>(m_config.view.height.value_or(kDefaultViewHeight));
  }
  // DRM/software report native mode dims (already physical, scale 1); a
  // Wayland window reports logical dims — convert with its surface scale so
  // Flutter sees the display in physical pixels (issue #150).
  double scale = 1.0;
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
  if (m_wayland_window) {
    scale = m_wayland_window->GetScale();
  }
#endif
  display.width = static_cast<size_t>(std::lround(width * scale));
  display.height = static_cast<size_t>(std::lround(height * scale));
  display.device_pixel_ratio =
      m_config.view.pixel_ratio.value_or(kDefaultPixelRatio) * scale;
  LibFlutterEngine->NotifyDisplayUpdate(m_flutter_engine->GetFlutterEngine(),
                                        kFlutterEngineDisplaysUpdateTypeStartup,
                                        &display, 1);
  ihs::log::info("Display metadata: {}x{} logical -> {}x{} px, pixel_ratio={}",
                 width, height, display.width, display.height,
                 display.device_pixel_ratio);

  // Update for Binary Messenger
  m_state->engine_state->flutter_engine = m_flutter_engine->GetFlutterEngine();
  m_state->engine_state->platform_task_runner =
      m_flutter_engine->GetPlatformTaskRunner();

  // Wire engine and text input plugin into the key event handler now that the
  // engine is running and the platform task runner is available.
  // Use the cached raw pointer set in the constructor instead of a
  // dynamic_cast + assert that silently breaks in release builds on re-order.
  m_key_event_handler->SetEngine(m_flutter_engine.get());
  m_key_event_handler->SetTextInputPlugin(m_state->text_input_plugin.get());

  // update view
  m_state->view = m_state->view_wrapper->view = this;

  // Runtime backend dispatch for first-frame wiring. Wayland: the
  // WaylandWindow's xdg configure triggers the initial window-metrics event, so
  // just route engine events by surface pointer. DRM/software: there is no
  // WaylandWindow, so Flutter never learns the viewport size on its own — send
  // the window metrics explicitly or the first frame is never scheduled.
  bool handled_by_wayland = false;
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
  if (m_wayland_window) {
    if (auto* wl = dynamic_cast<Display*>(m_display.get())) {
      wl->SetEngine(m_wayland_window->GetBaseSurface(), m_flutter_engine.get());
    }
    m_wayland_window->SetEngine(m_flutter_engine);
    handled_by_wayland = true;
  }
#endif
  if (!handled_by_wayland) {
    const auto result = m_flutter_engine->SetWindowSize(
        static_cast<size_t>(height), static_cast<size_t>(width));
    ihs::log::info("[FlutterView] SendWindowMetrics {}x{} result={}", width,
                   height, static_cast<int>(result));
#if BUILD_BACKEND_SOFTWARE
    // Match the seat's pointer-clamp viewport to the rendered size so the
    // pointer (and the software cursor) share the framebuffer coordinate space,
    // and hand the display's cursor to the sink (which composites it).
    if (auto* sw_display = dynamic_cast<SoftwareDisplay*>(m_display.get())) {
      sw_display->SetViewportSize(width, height);
      if (auto* sw_backend = dynamic_cast<SoftwareBackend*>(m_backend.get())) {
        sw_backend->SetCursor(sw_display->cursor());
      }
    }
#endif
  }

#if BUILD_BACKEND_DRM_KMS_EGL
  // this one engine drives additional outputs on the same DRM card.
  // Each was set up as a compositor bound to view id 1..N in the backend; now
  // that the engine is running, attach those views so Flutter builds and
  // presents them. The backend's present_view_callback routes each view to its
  // compositor/CRTC.
  if (auto* drm = dynamic_cast<DrmBackend*>(m_backend.get())) {
    const double pixel_ratio = m_flutter_engine->GetPixelRatio();
    for (const auto& v : drm->AdditionalViews()) {
      const auto r = m_flutter_engine->AddView(v.view_id, v.width, v.height,
                                               pixel_ratio, /*display_id=*/0);
      ihs::log::info("[FlutterView] AddView id={} {}x{} result={}", v.view_id,
                     v.width, v.height, static_cast<int>(r));
    }
  }
#endif

  IHS_DEBUG("({}) Engine running...", m_index);
}

void FlutterView::UpdateDisplayMetadata() const {
  // Only the Wayland path has a runtime scale source; DRM/software report
  // native mode dimensions once at startup and never rescale.
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
  if (!m_wayland_window || !m_flutter_engine ||
      !m_flutter_engine->IsRunning()) {
    return;
  }
  const auto [width, height] = m_wayland_window->GetSize();
  const double scale = m_wayland_window->GetScale();

  FlutterEngineDisplay display{};
  display.struct_size = sizeof(FlutterEngineDisplay);
  display.display_id = 1;
  display.single_display = true;
  display.refresh_rate =
      m_display->GetRefreshRate(m_wayland_window->GetOutputIndex());
  display.width = static_cast<size_t>(std::lround(width * scale));
  display.height = static_cast<size_t>(std::lround(height * scale));
  display.device_pixel_ratio =
      m_config.view.pixel_ratio.value_or(kDefaultPixelRatio) * scale;

  LibFlutterEngine->NotifyDisplayUpdate(m_flutter_engine->GetFlutterEngine(),
                                        kFlutterEngineDisplaysUpdateTypeStartup,
                                        &display, 1);
  ihs::log::debug("Display metadata: {}x{} logical -> {}x{} px, pixel_ratio={}",
                  width, height, display.width, display.height,
                  display.device_pixel_ratio);
#endif
}

void FlutterView::RunTasks() {
  // The Flutter platform task runner drains expired engine tasks on its own
  // asio strand (see TaskRunner::QueueFlutterTask), so there is nothing to pump
  // for the engine here.
#ifdef ENABLE_PLUGIN_COMP_SURF
  for (auto const& surface : m_comp_surf) {
    surface.second->RunTask();
  }
#endif

  // Flush any coalesced pointer events on every wake. The main loop is now
  // event-driven (woken by Engine::CoalesceMouseEvent/CoalesceTouchEvent), so
  // there is no per-frame busy-loop to throttle against — flushing
  // immediately minimises input latency, and SendPointerEvents() no-ops when
  // the queue is empty. Events that arrive in a burst are still coalesced in
  // Engine::m_pointer_events between wakes.
  m_flutter_engine->SendPointerEvents();
}

// Non-static by design: reads m_comp_surf when ENABLE_PLUGIN_COMP_SURF is on.
// In builds without it the body is constant, which clang-tidy flags — suppress
// it rather than splitting the definition per configuration.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
bool FlutterView::NeedsPeriodicPump() const {
#ifdef ENABLE_PLUGIN_COMP_SURF
  return !m_comp_surf.empty();
#else
  return false;
#endif
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

  // A surface may be created while the main loop is blocked idle; wake it so
  // it re-evaluates NeedsPeriodicPump() and starts ticking the plugin.
  MainLoopWaker::instance().Wake();

  const auto tEnd = std::chrono::steady_clock::now();
  const auto tDiff =
      std::chrono::duration<double, std::milli>(tEnd - tStart).count();
  ihs::log::info("comp surf init: {}", static_cast<float>(tDiff));

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
    IHS_DEBUG("Set Region: type: {}, x: {}, y: {}, width: {}, height: {}", type,
              region.x, region.y, region.width, region.height);
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
#if BUILD_WATCHDOG
void FlutterView::PetWatchdogViaCallback() {
  if (m_flutter_engine) {
    m_flutter_engine->PetWatchdogViaCallback();
  }
}
#endif