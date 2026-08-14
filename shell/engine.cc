// Copyright 2020 Toyota Connected North America
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

#include <pthread.h>
#include <sched.h>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

#include "logging/log_chunk.h"
#include "logging/logging.h"
#include "profiling/pv_latency.h"

#include <asio/post.hpp>

#include <dlfcn.h>
#include <cassert>

#include "config/common.h"
#include "engine.h"
#if BUILD_ACCESSIBILITY && BUILD_MCP
#include "ihs/ihs_mcp_registry.h"
#include "ihs/ihs_mcp_semantics.h"
#include "ihs/ihs_mcp_transport.h"
#endif

#if BUILD_ACCESSIBILITY
#include "ihs/ihs_semantics_host.h"
#include "shell/accessibility/semantics_action_args.h"
#include "shell/accessibility/semantics_publisher.h"
#endif
#include "hexdump.h"
#include "logging/logger.hpp"
#if BUILD_WATCHDOG
#include "watchdog/watchdog.h"
#endif
#include "main_loop_waker.h"
#include "utils.h"

#if BUILD_BACKEND_DRM_KMS_EGL
#include "backend/drm_kms_egl/drm_backend.h"
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"
#elif BUILD_BACKEND_DRM_KMS_VULKAN
// The Vulkan DRM backend reuses the DRM engine-state path but not the EGL
// backend header (which pulls in EGL/GL). Only the engine-state type is
// needed here.
#include "shell/platform/homescreen/flutter_desktop_engine_state.h"
#endif

extern void EngineOnFlutterPlatformMessage(
    const FlutterPlatformMessage* engine_message,
    void* user_data);

namespace {

// Wired into FlutterCustomTaskRunners.thread_priority_setter. Flutter
// calls this from each of its internal threads (raster, UI, IO) on
// thread startup, AND from the platform thread hosting the embedder-
// provided task runner — so we can target the threads that actually
// need to hit vblank deadlines (raster, display, platform-runner host)
// without touching DrmSession / DrmSeat / other infrastructure.
//
// Gated by IVI_DRM_RT (set to anything non-empty to enable). Default
// off because a SCHED_FIFO thread that spins is unrecoverable without
// a hard reset — dev-time safety. Once the code earns trust, flip the
// default by changing the env check.
//
// Requires CAP_SYS_NICE for the SCHED_FIFO paths. Grant it once on
// the binary:
//
//   sudo setcap cap_sys_nice=eip <binary>
//
// Without the capability pthread_setschedparam returns EPERM and the
// thread silently stays at SCHED_OTHER nice 0. Read the README if
// your IVI_DRM_RT flag isn't visibly helping.
extern "C" void EngineThreadPrioritySetter(FlutterThreadPriority prio) {
  static const bool enabled = std::getenv("IVI_DRM_RT") != nullptr;
  if (!enabled) {
    return;
  }
  struct sched_param p{};
  switch (prio) {
    case kRaster:
      // Builds frames + calls present_layers. Misses vblank → dropped
      // frame. Highest of our RT prios so it preempts the display
      // thread if they ever contend.
      //
      // Why prio 2 specifically — testing at prio 10 regressed 240Hz
      // cadence from 98% to 41% on amdgpu: raster preempted the
      // GPU-fence completion kthreads it was waiting on, so glFinish
      // spun on a fence that couldn't fire. Prio 2 lets ksoftirqd and
      // amdgpu kthreads (typically lower-RT or SCHED_OTHER) preempt
      // us, which is exactly what we want during glFinish.
      p.sched_priority = 2;
      pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
      break;
    case kDisplay:
      // UI / Dart thread. RT-class but one prio below raster.
      p.sched_priority = 1;
      pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
      break;
    case kBackground:
      // Cleanup / async work. SCHED_BATCH tells the kernel "don't
      // pick me for interactive responsiveness." No privilege needed.
      pthread_setschedparam(pthread_self(), SCHED_BATCH, &p);
      break;
    case kNormal:
    default:
      // Leave at SCHED_OTHER nice 0.
      break;
  }
}

}  // namespace

Engine::Engine(FlutterView* view,
               const size_t index,
               const std::vector<const char*>& command_line_args_c,
               const std::vector<const char*>& dart_entrypoint_args_c,
               const std::string& bundle_path,
               const int32_t accessibility_features,
               const bool merge_render_platform,
               const bool enable_mcp)
    : m_index(index),
      m_running(false),
      m_backend(view->GetBackend()),
      m_view(view),
      m_cache_path(GetFilePath(index)),
      m_prev_height(0),
      m_prev_width(0),
      m_prev_pixel_ratio(1.0),
      m_accessibility_features(accessibility_features),
      m_enable_mcp(enable_mcp),
      m_flutter_engine(nullptr) {
  IHS_TRACE("({}) +Engine::Engine", m_index);

  m_args.struct_size = sizeof(FlutterProjectArgs);
  m_args.command_line_argc = static_cast<int>(command_line_args_c.size());
  m_args.command_line_argv = command_line_args_c.data();
  m_args.dart_entrypoint_argc = static_cast<int>(dart_entrypoint_args_c.size());
  m_args.dart_entrypoint_argv = dart_entrypoint_args_c.data();
  m_args.platform_message_callback = OnFlutterPlatformMessage;
  m_args.persistent_cache_path = m_cache_path.c_str();
  m_args.is_persistent_cache_read_only = false;
  m_args.log_message_callback = onLogMessageCallback;
#if BUILD_ACCESSIBILITY
  m_args.update_semantics_callback2 = onSemanticsUpdateCallback;
#endif
  m_args.log_tag = "flutter";

  // Optional per-backend vsync_callback. nullptr (the default for
  // wayland_egl / wayland_vulkan) leaves the field unset
  // and Flutter falls back to its internal wall-clock scheduler.
  // DrmBackend overrides this to deliver vblank-locked OnVsync via
  // DRM PAGE_FLIP_EVENT (env-gated by IVI_DRM_VSYNC; set to 0 to
  // force wall-clock pacing for diagnostics).
  if (auto* vsync_cb = m_backend->GetVsyncCallback(); vsync_cb != nullptr) {
    m_args.vsync_callback = vsync_cb;
  }

  /// Task Runner
  m_platform_task_runner =
      std::make_shared<TaskRunner>("Platform", m_flutter_engine);

  // Touch events
  m_pointer_events.clear();
  m_pointer_events.reserve(kMaxPointerEvent);

  ///
  /// libflutter_engine.so loading
  ///
  if (bundle_path.empty()) {
    ihs::log::critical("Specify bundle folder using --b= option");
    exit(EXIT_FAILURE);
  }

  // Engine selection: a bundle-local engine overrides the system engine.
  // Overall precedence: preloaded > bundle > linker search (DT_RUNPATH,
  // LD_LIBRARY_PATH, ld.so.cache). The latter two are selected here; a
  // preloaded engine is detected inside Load() and wins over both.
  std::filesystem::path engine_file_path(bundle_path);
  engine_file_path /= kBundleEngine;
  if (std::filesystem::exists(engine_file_path)) {
    IHS_DEBUG("({}) libflutter_engine.so: {}", m_index,
              engine_file_path.c_str());
  } else {
    engine_file_path = kSystemEngine;
  }

  // One-shot per process: the first view binds the engine. Load() fails on
  // dlopen error, a missing required export, or a prior view having bound a
  // different path — each cause is already logged at critical/error by the
  // loader itself.
  if (!LibFlutterEngine::Load(engine_file_path.c_str())) {
    ihs::log::critical("({}) unable to load flutter engine: {}", m_index,
                       engine_file_path.c_str());
    exit(EXIT_FAILURE);
  }

  ///
  /// flutter_assets folder
  ///

  m_assets_path = bundle_path;
  m_assets_path /= kBundleFlutterAssets;
  IHS_DEBUG("({}) flutter_assets: {}", m_index, m_assets_path.c_str());
  m_args.assets_path = m_assets_path.c_str();

  ///
  /// icudtl.dat file
  ///
  m_icu_data_path = bundle_path;
  m_icu_data_path /= kBundleIcudtl;
  if (!exists(m_icu_data_path)) {
    m_icu_data_path = kPathPrefix;
    m_icu_data_path /= kSystemIcudtl;
  }
  if (!exists(m_icu_data_path)) {
    ihs::log::critical("({}) {} is not present.", m_index,
                       m_icu_data_path.c_str());
    assert(false);
  }
  IHS_DEBUG("({}) icudtl.dat: {}", m_index, m_icu_data_path.c_str());
  m_args.icu_data_path = m_icu_data_path.c_str();

  ///
  /// AOT loading
  ///
  if (LibFlutterEngine->RunsAOTCompiledDartCode()) {
    m_args.aot_data = nullptr;
    m_aot_data = LoadAotData(bundle_path);
    if (m_aot_data) {
      m_args.aot_data = m_aot_data;
    }
  } else {
    ihs::log::info("({}) Runtime=debug", m_index);
    std::filesystem::path kernel_snapshot = m_assets_path;
    kernel_snapshot /= "kernel_blob.bin";
    if (!exists(kernel_snapshot)) {
      ihs::log::critical("({}) {} missing Flutter Kernel\0", m_index,
                         kernel_snapshot.c_str());
      exit(EXIT_FAILURE);
    }
  }

  /// Configure task runner interop
  m_platform_task_runner_description = {};
  m_platform_task_runner_description.struct_size =
      sizeof(FlutterTaskRunnerDescription);
  m_platform_task_runner_description.user_data = this;
  m_platform_task_runner_description.runs_task_on_current_thread_callback =
      [](void* context) -> bool {
    const auto engine = static_cast<Engine*>(context);
    return engine->m_platform_task_runner->IsThreadEqual(pthread_self());
  };
  m_platform_task_runner_description.post_task_callback =
      [](const FlutterTask task, const uint64_t target_time,
         void* context) -> void {
    const auto engine = static_cast<Engine*>(context);
    engine->m_platform_task_runner->QueueFlutterTask(
        engine->m_index, target_time, task, context);
  };

  m_custom_task_runners = {};
  m_custom_task_runners.struct_size = sizeof(FlutterCustomTaskRunners);
  m_custom_task_runners.platform_task_runner =
      &m_platform_task_runner_description;

  // Optionally run the raster thread on the platform thread: a render task
  // runner whose identifier matches the platform one makes the engine collapse
  // them (embedder_thread_host identifier equality), so the rasterizer + GL /
  // compositor callbacks run on the platform thread. UI and IO stay separate.
  // The present path must stay non-blocking on the platform thread for this to
  // be safe (an independent per-card flip reader delivers completion).
  m_merge_render_platform = merge_render_platform;
  if (m_merge_render_platform) {
    constexpr size_t kMergedRunnerId = 1;
    m_platform_task_runner_description.identifier = kMergedRunnerId;
    m_render_task_runner_description = m_platform_task_runner_description;
    m_custom_task_runners.render_task_runner =
        &m_render_task_runner_description;
    ihs::log::info("({}) engine: raster thread merged onto the platform thread",
                   m_index);
  }

  // Per-thread priority setter (opt-in via IVI_DRM_RT=1). See
  // EngineThreadPrioritySetter for the mapping rationale.
  m_custom_task_runners.thread_priority_setter = &EngineThreadPrioritySetter;

  m_args.custom_task_runners = &m_custom_task_runners;

  IHS_TRACE("({}) -Engine::Engine", m_index);
}

Engine::~Engine() {
  // Stop the engine (idempotent) as a fallback for owners that don't drive
  // the explicit shutdown sequence in FlutterView::~FlutterView.
  Shutdown();
  // Free engine_state explicitly here — after Deinitialize/Shutdown have
  // joined all engine threads and drained final callbacks (so user_data
  // dereferences in OnFlutterPlatformMessage hit live state), but before
  // m_platform_task_runner.reset() destroys the task runner that
  // engine_state→messenger holds pointers into.
  m_engine_state.reset();
  m_platform_task_runner.reset();
}

void Engine::Shutdown() {
  if (!m_running) {
    return;  // never started, or already stopped — idempotent
  }
  m_running = false;

#if BUILD_ACCESSIBILITY && BUILD_MCP
  // Before the engine threads go: stop() guarantees no provider callback is in
  // flight when it returns, and releases the hub registration that was keeping
  // semantics on. Safe when start never ran.
  if (m_enable_mcp) {
    // Transport first: it routes into the provider, so stopping the provider
    // while a request was in flight would tear out what that request is
    // standing on. Stop guarantees nothing is in flight when it returns.
    ihs_mcp_transport_stop();
    ihs_mcp_semantics_provider_stop();
  }
#endif
  // Deinitialize stops new frame work; Shutdown joins the platform, UI and
  // rasterizer threads. After this returns nothing in the engine touches the
  // backend (no more VsyncTrampoline / PresentLayers), so the caller may
  // safely release the GL contexts and render surfaces those threads used.
  if (m_flutter_engine) {
    LibFlutterEngine->Deinitialize(m_flutter_engine);
    LibFlutterEngine->Shutdown(m_flutter_engine);
  }
  if (m_aot_data) {
    LibFlutterEngine->CollectAOTData(m_aot_data);
    m_aot_data = nullptr;
  }
}

bool Engine::IsRunning() const {
  return m_running;
}

FlutterEngineResult Engine::SendKeyEvent(const FlutterKeyEvent& event,
                                         FlutterKeyEventCallback callback,
                                         void* user_data) const {
  if (!m_running) {
    return kInternalInconsistency;
  }
  return LibFlutterEngine->SendKeyEvent(m_flutter_engine, &event, callback,
                                        user_data);
}

FlutterEngineResult Engine::Run(FlutterDesktopEngineState* state) {
  IHS_TRACE("({}) +Engine::Run", m_index);

  const auto config = m_backend->GetRenderConfig();
  m_compositor = m_backend->GetCompositorConfig();
  if (m_compositor.present_layers_callback != nullptr ||
      m_compositor.create_backing_store_callback != nullptr) {
    m_args.compositor = &m_compositor;
  }
  FlutterEngineResult result = LibFlutterEngine->Initialize(
      FLUTTER_ENGINE_VERSION, &config, &m_args, state, &m_flutter_engine);
  if (result != kSuccess) {
    ihs::log::error("({}) FlutterEngineRun failed or engine is null", m_index);
    return result;
  }

  result = LibFlutterEngine->RunInitialized(m_flutter_engine);
  if (result == kSuccess) {
    m_running = true;
    IHS_DEBUG("({}) Engine::m_running = {}", m_index, m_running);
  }

  // Set available system locales
  SetUpLocales();

  // Set Accessibility Features
  LibFlutterEngine->UpdateAccessibilityFeatures(
      m_flutter_engine,
      static_cast<FlutterAccessibilityFeature>(m_accessibility_features));

#if BUILD_ACCESSIBILITY
  // Semantics is no longer switched on unconditionally. The hub reference-
  // counts its consumers and asks for it through this host, so a build with
  // the subsystem compiled in but nobody listening costs the engine nothing.
  InstallSemanticsHost();
#if ENABLE_ACCESSKIT
  // Registering is what asks the hub to turn semantics on, so the bridge is
  // created after the host is installed and not before.
  m_accesskit = std::make_unique<accessibility::AccessKitConsumer>();
#endif
#if BUILD_MCP
  // The second half of the double opt-in: the surface is compiled in, and the
  // image asked for it. Starting registers a hub consumer, which is what turns
  // semantics on, so leaving it off really does cost nothing rather than
  // merely hiding the tools.
  if (m_enable_mcp) {
    const int status = ihs_mcp_semantics_provider_start();
    if (status == IHS_MCP_OK) {
      ihs::log::info("({}) MCP semantics provider started", m_index);
      // Only once there is something to serve: the transport routes onto the
      // registry, so binding a socket before the provider registered would
      // advertise an empty tool list to whoever connected first.
      IhsMcpTransportConfig transport{};
      transport.struct_size = sizeof(transport);
      const int served = ihs_mcp_transport_start(&transport);
      if (served == IHS_MCP_OK) {
        ihs::log::info("({}) MCP serving on {}", m_index,
                       ihs_mcp_transport_socket_path());
      } else {
        ihs::log::error("({}) MCP transport failed to start: {}", m_index,
                        served);
      }
    } else {
      // Not fatal: the UI is the product and it runs without an agent
      // attached. Loud, though, because the image explicitly asked for this.
      ihs::log::error("({}) MCP semantics provider failed to start: {}",
                      m_index, status);
    }
  }
#endif
#endif

  IHS_TRACE("({}) -Engine::Run", m_index);
  return result;
}

FlutterEngineResult Engine::SetWindowSize(const size_t height,
                                          const size_t width) {
  if (!m_running) {
    return kInternalInconsistency;
  }

  m_prev_height = height;
  m_prev_width = width;

  // Set window size
  const FlutterWindowMetricsEvent fwme = {
      .struct_size = sizeof(FlutterWindowMetricsEvent),
      .width = width,
      .height = height,
      .pixel_ratio = m_prev_pixel_ratio,
      .left = 0,
      .top = 0,
      .physical_view_inset_top = 0,
      .physical_view_inset_right = 0,
      .physical_view_inset_bottom = 0,
      .physical_view_inset_left = 0,
      .display_id = 0,  // TODO display index
      // Each bundle is a discrete engine whose implicit view is id 0. m_index
      // identifies the view within App, not within this engine -- sending it as
      // view_id targeted a non-existent view on every engine past the first, so
      // that engine never scheduled a frame. One engine driving multiple views
      // (FlutterEngineAddView) would use the real per-view ids here.
      .view_id = 0,
      // The engine gained optional size constraints; this port does not
      // negotiate them, so declare their absence explicitly rather than
      // leaving the trailing members to implicit initialization.
      .has_constraints = false,
      .min_width_constraint = 0,
      .min_height_constraint = 0,
      .max_width_constraint = 0,
      .max_height_constraint = 0};

  if (LibFlutterEngine->SendWindowMetricsEvent(m_flutter_engine, &fwme) !=
      kSuccess) {
    ihs::log::critical("({}) Failed send initial window size to flutter",
                       m_index);
    assert(false);
  }

  return kSuccess;
}

void Engine::OnAddViewComplete(const FlutterAddViewResult* result) {
  if (result == nullptr) {
    return;
  }
  const auto* self = static_cast<const Engine*>(result->user_data);
  const size_t idx = self != nullptr ? self->m_index : 0;
  if (!result->added) {
    ihs::log::error("({}) FlutterEngineAddView failed", idx);
  } else {
    ihs::log::debug("({}) view added", idx);
  }
}

void Engine::OnRemoveViewComplete(const FlutterRemoveViewResult* result) {
  if (result == nullptr) {
    return;
  }
  const auto* self = static_cast<const Engine*>(result->user_data);
  const size_t idx = self != nullptr ? self->m_index : 0;
  if (!result->removed) {
    ihs::log::error("({}) FlutterEngineRemoveView failed", idx);
  } else {
    ihs::log::debug("({}) view removed", idx);
  }
}

FlutterEngineResult Engine::AddView(const int64_t view_id,
                                    const size_t width,
                                    const size_t height,
                                    const double pixel_ratio,
                                    const uint64_t display_id) {
  if (!m_running) {
    return kInternalInconsistency;
  }
  if (LibFlutterEngine->AddView == nullptr) {
    ihs::log::error(
        "({}) engine library has no FlutterEngineAddView; multi-view "
        " is unavailable",
        m_index);
    return kInternalInconsistency;
  }
  // view_metrics is copied synchronously inside FlutterEngineAddView (the info
  // may be freed once it returns), so a local is safe. The metric's view_id
  // must match the info's view_id.
  const FlutterWindowMetricsEvent metrics = {
      .struct_size = sizeof(FlutterWindowMetricsEvent),
      .width = width,
      .height = height,
      .pixel_ratio = pixel_ratio,
      .left = 0,
      .top = 0,
      .physical_view_inset_top = 0,
      .physical_view_inset_right = 0,
      .physical_view_inset_bottom = 0,
      .physical_view_inset_left = 0,
      .display_id = display_id,
      .view_id = view_id,
      // The engine gained optional size constraints; this port does not
      // negotiate them, so declare their absence explicitly rather than
      // leaving the trailing members to implicit initialization.
      .has_constraints = false,
      .min_width_constraint = 0,
      .min_height_constraint = 0,
      .max_width_constraint = 0,
      .max_height_constraint = 0};
  const FlutterAddViewInfo info = {
      .struct_size = sizeof(FlutterAddViewInfo),
      .view_id = view_id,
      .view_metrics = &metrics,
      .user_data = this,
      .add_view_callback = &Engine::OnAddViewComplete};
  return LibFlutterEngine->AddView(m_flutter_engine, &info);
}

FlutterEngineResult Engine::RemoveView(const int64_t view_id) {
  if (!m_running) {
    return kInternalInconsistency;
  }
  if (LibFlutterEngine->RemoveView == nullptr) {
    return kInternalInconsistency;
  }
  const FlutterRemoveViewInfo info = {
      .struct_size = sizeof(FlutterRemoveViewInfo),
      .view_id = view_id,
      .user_data = this,
      .remove_view_callback = &Engine::OnRemoveViewComplete};
  return LibFlutterEngine->RemoveView(m_flutter_engine, &info);
}

FlutterEngineResult Engine::SetPixelRatio(double pixel_ratio) {
  if (!m_running) {
    return kInternalInconsistency;
  }

  assert(m_prev_width);
  assert(m_prev_height);

  m_prev_pixel_ratio = pixel_ratio;

  // Set window size
  const FlutterWindowMetricsEvent fwme = {
      .struct_size = sizeof(FlutterWindowMetricsEvent),
      .width = m_prev_width,
      .height = m_prev_height,
      .pixel_ratio = pixel_ratio,
      .left = 0,
      .top = 0,
      .physical_view_inset_top = 0,
      .physical_view_inset_right = 0,
      .physical_view_inset_bottom = 0,
      .physical_view_inset_left = 0,
      .display_id = 0,  // TODO display index
      // Each bundle is a discrete engine whose implicit view is id 0. m_index
      // identifies the view within App, not within this engine -- sending it as
      // view_id targeted a non-existent view on every engine past the first, so
      // that engine never scheduled a frame. One engine driving multiple views
      // (FlutterEngineAddView) would use the real per-view ids here.
      .view_id = 0,
      // The engine gained optional size constraints; this port does not
      // negotiate them, so declare their absence explicitly rather than
      // leaving the trailing members to implicit initialization.
      .has_constraints = false,
      .min_width_constraint = 0,
      .min_height_constraint = 0,
      .max_width_constraint = 0,
      .max_height_constraint = 0};

  const auto result =
      LibFlutterEngine->SendWindowMetricsEvent(m_flutter_engine, &fwme);
  if (result != kSuccess) {
    ihs::log::critical("({}) Failed send initial window size to flutter",
                       m_index);
    assert(false);
  }

  IHS_TRACE("({}) SetWindowSize: width={}, height={}, pixel_ratio={}", m_index,
            m_prev_width, m_prev_height, pixel_ratio);
  return kSuccess;
}

std::string Engine::GetFilePath(size_t index) {
  auto path = Utils::GetConfigHomePath();

  if (!std::filesystem::is_directory(path) || !std::filesystem::exists(path)) {
    if (!std::filesystem::create_directories(path)) {
      if (!std::filesystem::is_directory(path)) {
        ihs::log::critical("({}) create_directories failed: {}", index, path);
        exit(EXIT_FAILURE);
      }
    }
  }

  IHS_DEBUG("({}) PersistentCachePath: {}", index, path);

  return path;
}

FlutterEngineResult Engine::SendPlatformMessageResponse(
    const FlutterPlatformMessageResponseHandle* handle,
    const uint8_t* data,
    size_t data_length) const {
  if (!m_running) {
    return kInternalInconsistency;
  }

  if (!m_platform_task_runner->IsThreadEqual(pthread_self())) {
    ihs::log::error("Not sending message on Platform Thread");
  }

  return LibFlutterEngine->SendPlatformMessageResponse(m_flutter_engine, handle,
                                                       data, data_length);
}

bool Engine::SendPlatformMessage(
    const char* channel,
    std::unique_ptr<std::vector<uint8_t>> message,
    const FlutterPlatformMessageResponseHandle* response_handle) const {
  if (!m_running) {
    return false;
  }

  FlutterEngineResult result;
  if (!m_platform_task_runner->IsThreadEqual(pthread_self())) {
    auto f = m_platform_task_runner->QueuePlatformMessage(channel,
                                                          std::move(message));
    f.wait();
    result = f.get();
  } else {
    const FlutterPlatformMessage msg{sizeof(FlutterPlatformMessage), channel,
                                     message->data(), message->size(),
                                     response_handle};
    result = LibFlutterEngine->SendPlatformMessage(m_flutter_engine, &msg);
  }

  return (result == kSuccess);
}

bool Engine::SendPlatformMessage(const char* channel,
                                 const uint8_t* message,
                                 const size_t message_size) const {
  if (!m_running) {
    return false;
  }

  FlutterEngineResult result;
  if (!m_platform_task_runner->IsThreadEqual(pthread_self())) {
    auto msg =
        std::make_unique<std::vector<uint8_t>>(message, message + message_size);
    auto f =
        m_platform_task_runner->QueuePlatformMessage(channel, std::move(msg));
    f.wait();
    result = f.get();
  } else {
    const FlutterPlatformMessage msg{sizeof(FlutterPlatformMessage), channel,
                                     message, message_size, nullptr};
    result = LibFlutterEngine->SendPlatformMessage(m_flutter_engine, &msg);
  }
  return (result == kSuccess);
}

bool Engine::SendPlatformMessage(const char* channel,
                                 const uint8_t* message,
                                 const size_t message_size,
                                 const FlutterDataCallback reply,
                                 void* userdata) const {
  if (!m_running) {
    return false;
  }
  FlutterPlatformMessageResponseHandle* handle;
  LibFlutterEngine->PlatformMessageCreateResponseHandle(m_flutter_engine, reply,
                                                        userdata, &handle);

  FlutterEngineResult result;
  if (!m_platform_task_runner->IsThreadEqual(pthread_self())) {
    auto msg =
        std::make_unique<std::vector<uint8_t>>(message, message + message_size);
    auto f = m_platform_task_runner->QueuePlatformMessage(
        channel, std::move(msg), handle);
    f.wait();
    result = f.get();
  } else {
    const FlutterPlatformMessage msg{
        sizeof(FlutterPlatformMessage), channel, message, message_size, handle,
    };

    result = LibFlutterEngine->SendPlatformMessage(m_flutter_engine, &msg);
  }
  if (handle != nullptr) {
    LibFlutterEngine->PlatformMessageReleaseResponseHandle(m_flutter_engine,
                                                           handle);
  }

  return result == kSuccess;
}

[[maybe_unused]] FlutterEngineResult Engine::UpdateAccessibilityFeatures(
    int32_t value) {
  m_accessibility_features = value;
  return LibFlutterEngine->UpdateAccessibilityFeatures(
      m_flutter_engine, static_cast<FlutterAccessibilityFeature>(value));
}

// Passes locale information to the Flutter engine.
void Engine::SetUpLocales() const {
  constexpr FlutterLocale locale = {.struct_size = sizeof(FlutterLocale),
                                    .language_code = kDefaultLocaleLanguageCode,
                                    .country_code = kDefaultLocaleCountryCode,
                                    .script_code = kDefaultLocaleScriptCode,
                                    .variant_code = nullptr};

  std::vector<const FlutterLocale*> flutter_locale_list;
  flutter_locale_list.push_back(&locale);

  FlutterEngineResult result;
  if (!m_platform_task_runner->IsThreadEqual(pthread_self())) {
    auto f = m_platform_task_runner->QueueUpdateLocales(
        std::move(flutter_locale_list));
    f.wait();
    result = f.get();
  } else {
    result = LibFlutterEngine->UpdateLocales(m_flutter_engine,
                                             flutter_locale_list.data(),
                                             flutter_locale_list.size());
  }

  if (result != kSuccess) {
    ihs::log::error("({}) Failed to set up Flutter locales.", m_index);
  }
}

void Engine::CoalesceMouseEvent(const FlutterPointerSignalKind signal,
                                const FlutterPointerPhase phase,
                                const double x,
                                const double y,
                                const double scroll_delta_x,
                                const double scroll_delta_y,
                                const int64_t buttons,
                                const uint64_t timestamp_us) {
  // 0 = no high-resolution source; stamp with the arrival time (the engine
  // clock is CLOCK_MONOTONIC, so a caller-supplied kernel input timestamp in
  // the same domain slots in directly and predates this by the input path's
  // latency — better data for the framework's velocity tracker/resampler).
  const auto timestamp = timestamp_us != 0
                             ? timestamp_us
                             : LibFlutterEngine->GetCurrentTime() / 1000;

  // Latency probe: a tap fires down then up; stamp both so the last one before
  // a platform-view create burst is the trigger (see pv_latency.h).
  if (phase == kDown || phase == kUp) {
    ivi::pv_latency::MarkInput();
  }

  std::scoped_lock lock(m_pointer_mutex);

  FlutterPointerEvent e{};
  e.struct_size = sizeof(FlutterPointerEvent);
  e.phase = phase;
#if ENV64BIT
  e.timestamp = timestamp;
#elif ENV32BIT
  e.timestamp = static_cast<size_t>(timestamp & 0xFFFFFFFFULL);
#endif
  e.x = x * m_pointer_scale;
  e.y = y * m_pointer_scale;
  e.device = 0;
  e.signal_kind = signal;
  e.scroll_delta_x = scroll_delta_x * m_pointer_scale;
  e.scroll_delta_y = scroll_delta_y * m_pointer_scale;
  e.device_kind = kFlutterPointerDeviceKindMouse;
  e.buttons = buttons;
  e.pan_x = 0;
  e.pan_y = 0;
  e.scale = 0;
  e.rotation = 0;

  m_pointer_events.emplace_back(e);

  // Wake the main loop so it flushes this batch promptly instead of waiting
  // for its next periodic tick (the loop blocks idle on a static screen).
  MainLoopWaker::instance().Wake();
}

void Engine::CoalesceTouchEvent(const FlutterPointerPhase phase,
                                const double x,
                                const double y,
                                const int32_t device) {
  const TouchEvent e{phase, x, y, device};
  CoalesceTouchFrame(&e, 1);
}

void Engine::CoalesceTouchFrame(const TouchEvent* events,
                                const size_t count,
                                const uint64_t timestamp_us) {
  if (events == nullptr || count == 0) {
    return;
  }

  // One timestamp for the whole frame: the contacts in a touch frame are
  // logically simultaneous (wl_touch.frame / libinput TOUCH_FRAME group one
  // hardware scan), and a shared timestamp keeps the engine's pointer
  // resampler from interpolating skew between fingers that moved together.
  // 0 = no high-resolution source; stamp with the arrival time.
  const auto timestamp = timestamp_us != 0
                             ? timestamp_us
                             : LibFlutterEngine->GetCurrentTime() / 1000;

  std::scoped_lock lock(m_pointer_mutex);
  for (size_t i = 0; i < count; ++i) {
    FlutterPointerEvent e{};
    e.struct_size = sizeof(FlutterPointerEvent);
    e.phase = events[i].phase;
#if ENV64BIT
    e.timestamp = timestamp;
#elif ENV32BIT
    e.timestamp = static_cast<size_t>(timestamp & 0xFFFFFFFFULL);
#endif
    e.x = events[i].x * m_pointer_scale;
    e.y = events[i].y * m_pointer_scale;
    e.device = events[i].device;
    e.signal_kind = kFlutterPointerSignalKindNone;
    e.scroll_delta_x = 0.0;
    e.scroll_delta_y = 0.0;
    e.device_kind = kFlutterPointerDeviceKindTouch;
    e.buttons = 0;
    e.pan_x = 0;
    e.pan_y = 0;
    e.scale = 0;
    e.rotation = 0;

    m_pointer_events.emplace_back(e);
  }

  // Wake the main loop once per frame so it flushes this batch promptly
  // instead of waiting for its next periodic tick (the loop blocks idle on a
  // static screen). Per-event wakes on a 10-finger controller would be ten
  // eventfd writes per hardware scan for one flush.
  MainLoopWaker::instance().Wake();
}

void Engine::SendPointerEvents() {
  if (m_flutter_engine) {
    std::scoped_lock lock(m_pointer_mutex);
    // Emptiness is checked under the lock, not before it. The main loop
    // flushes this queue as well as the caller, so with the test outside both
    // could pass it and the loser would hand the engine a zero-length batch,
    // which it rejects as invalid arguments.
    if (m_pointer_events.empty()) {
      return;
    }
    LibFlutterEngine->SendPointerEvent(
        m_flutter_engine, m_pointer_events.data(), m_pointer_events.size());
    m_pointer_events.clear();
  }
}

FlutterEngineAOTData Engine::LoadAotData(const std::string& bundle_path) const {
  std::filesystem::path aot_data_path(bundle_path);
  aot_data_path /= kBundleAot;
  if (!exists(aot_data_path)) {
    IHS_DEBUG("({}) AOT file not present", m_index);
    return nullptr;
  }

  ihs::log::info("({}) Loading AOT: {}", m_index, aot_data_path.c_str());

  FlutterEngineAOTDataSource source = {};
  source.type = kFlutterEngineAOTDataSourceTypeElfPath;
  source.elf_path = aot_data_path.c_str();

  FlutterEngineAOTData data;
  if (kSuccess != LibFlutterEngine->CreateAOTData(&source, &data)) {
    ihs::log::critical("({}) Failed to load AOT data from: {}", m_index,
                       aot_data_path.c_str());
    return nullptr;
  }
  return data;
}

void Engine::OnFlutterPlatformMessage(
    const FlutterPlatformMessage* engine_message,
    void* user_data) {
  if (engine_message->struct_size != sizeof(FlutterPlatformMessage)) {
    ihs::log::error(
        "Invalid message size received. Expected: {} but received {}",
        sizeof(FlutterPlatformMessage), engine_message->struct_size);
    return;
  }

  FlutterDesktopEngineState const* engine_state =
      static_cast<FlutterDesktopEngineState*>(user_data);

  auto* view = engine_state->view_controller == nullptr
                   ? nullptr
                   : engine_state->view_controller->view;

#if DEBUG_PLATFORM_MESSAGES
  std::stringstream ss;
  ss << Hexdump(engine_message->message, engine_message->message_size);
  ihs::log::debug("Channel: \"{}\"\n{}", engine_message->channel, ss.str());
#endif

  engine_state->message_dispatcher->HandleMessage(
      {.struct_size = sizeof(FlutterDesktopMessage),
       .channel = engine_message->channel,
       .message = engine_message->message,
       .message_size = engine_message->message_size,
       .response_handle = engine_message->response_handle},
      [view] {
        if (view) {
          IHS_TRACE("input_block_cb");
        }
      },
      [view] {
        if (view) {
          IHS_TRACE("input_unblock_cb");
        }
      });
}

void Engine::onLogMessageCallback(const char* tag,
                                  const char* message,
                                  void* /* user_data */) {
  // One record per line. Dart print() and debugPrint() arrive here whole, and
  // a stack trace is several lines in one string; without this they run
  // together into a single record. Over-length lines need no handling here --
  // the logging library splits and the drain rejoins them.
  const std::string_view tag_text = tag != nullptr ? tag : "";
  ihs::log::emit_chunked(message, [tag_text](std::string_view line) {
    ihs::log::info("{}: {}", tag_text, line);
  });

#if ENABLE_DLT
  // Route the Flutter engine's message to the DLT bridge under a dedicated
  // context, via the ihs_shared C ABI. The context is acquired on first call
  // and cached for the process lifetime by the bridge.
  static IhsLogContext kEngineCtx("FLTR", "Flutter engine");
  if (kEngineCtx.is_valid()) {
    char buf[256];
    const int n = std::snprintf(buf, sizeof(buf), "%s: %s", tag ? tag : "",
                                message ? message : "");
    if (n > 0) {
      const auto len =
          static_cast<std::size_t>(n < static_cast<int>(sizeof(buf))
                                       ? n
                                       : static_cast<int>(sizeof(buf)) - 1);
      ihs_log(kEngineCtx.index(), IHS_LEVEL_INFO, buf, len);
    }
  }
#endif
}

#if BUILD_ACCESSIBILITY
void Engine::InstallSemanticsHost() {
  IhsSemanticsHost host{};
  host.struct_size = sizeof(IhsSemanticsHost);
  host.user_data = this;

  host.set_semantics_enabled = [](void* user_data, const bool enabled) {
    auto* engine = static_cast<Engine*>(user_data);
    if (engine->m_flutter_engine == nullptr) {
      return;
    }
    LibFlutterEngine->UpdateSemanticsEnabled(engine->m_flutter_engine, enabled);
  };

  host.dispatch = [](void* user_data, const int64_t view_id,
                     const int32_t node_id, const uint64_t action,
                     const uint8_t* data, const size_t data_length) -> int {
    auto* engine = static_cast<Engine*>(user_data);
    return engine->DispatchSemanticsAction(view_id, node_id, action, data,
                                           data_length);
  };

  host.send_pointer_tap = [](void* user_data, const int64_t /* view_id */,
                             const double x, const double y) -> int {
    auto* engine = static_cast<Engine*>(user_data);
    return engine->SendSyntheticTap(x, y);
  };

  ihs_semantics_set_host(&host);
}

int Engine::SendSyntheticTap(const double x, const double y) {
  if (m_flutter_engine == nullptr) {
    return IHS_SEMANTICS_ERR_UNAVAILABLE;
  }

  // Posted rather than sent here: the coalescing buffer is drained on the
  // platform thread, and a caller may be on any thread.
  asio::post(*m_platform_task_runner->GetStrandContext(), [this, x, y]() {
    if (m_flutter_engine == nullptr) {
      return;
    }
    // Add, then down, then up -- the sequence a real seat produces. Sent
    // through the ordinary coalescing path rather than around it, so a
    // synthesized tap is hit-tested, scaled and batched exactly as a physical
    // one is; anything else would make the fallback behave unlike the input
    // it stands in for.
    CoalesceMouseEvent(kFlutterPointerSignalKindNone, kAdd, x, y, 0.0, 0.0, 0,
                       0);
    CoalesceMouseEvent(kFlutterPointerSignalKindNone, kDown, x, y, 0.0, 0.0,
                       kFlutterPointerButtonMousePrimary, 0);
    SendPointerEvents();
    // Release in a later task, not the same batch. A down and an up delivered
    // in one packet are one frame's worth of input with no interval between
    // them, which is not a gesture any recognizer is looking for.
    asio::post(*m_platform_task_runner->GetStrandContext(), [this, x, y]() {
      if (m_flutter_engine == nullptr) {
        return;
      }
      CoalesceMouseEvent(kFlutterPointerSignalKindNone, kUp, x, y, 0.0, 0.0,
                         kFlutterPointerButtonMousePrimary, 0);
      SendPointerEvents();
    });
  });

  // Accepted for delivery. Whether anything was under the point is not
  // knowable here, and reporting success as though it were would be the
  // silent-guess behaviour this path is deliberately not: a coordinate tap is
  // asked for explicitly, never substituted for a failed lookup.
  return IHS_SEMANTICS_OK;
}

int Engine::DispatchSemanticsAction(const int64_t view_id,
                                    const int32_t node_id,
                                    const uint64_t action,
                                    const uint8_t* data,
                                    const size_t data_length) {
  if (m_flutter_engine == nullptr) {
    return IHS_SEMANTICS_ERR_UNAVAILABLE;
  }

  const FlutterSemanticsAction fl_action =
      accessibility::ToFlutterSemanticsAction(action);
  if (fl_action == static_cast<FlutterSemanticsAction>(0)) {
    // The hub's action bits are its own; one with no framework equivalent is
    // a translation gap, not something to forward as a zero mask.
    return IHS_SEMANTICS_ERR_UNSUPPORTED_ACTION;
  }

  // Encode here rather than in the hub: `data` arrives in the hub's own plain
  // layout, and this is the only side that links Flutter, so consumers need no
  // codec and the hub ABI keeps Flutter's wire format out of it.
  auto encoded =
      accessibility::EncodeActionArguments(action, data, data_length);
  if (!encoded.has_value()) {
    // The argument did not match what the action takes. Refusing beats
    // forwarding: a malformed payload is dropped framework-side in silence,
    // so the caller would see a successful dispatch that did nothing.
    return IHS_SEMANTICS_ERR_INVALID;
  }

  // The hub does not thread-hop -- deliberately, since the task runner is
  // ours. The encoded buffer moves with the post: `data` is only valid for
  // this call, and the engine reads the bytes on the platform thread after.
  std::vector<uint8_t> payload = std::move(encoded.value());

  // The strand is serviced by the same thread that runs Flutter tasks, so
  // posting here lands the call on the platform thread with no extra
  // synchronization.
  asio::post(*m_platform_task_runner->GetStrandContext(),
             [this, view_id, node_id, fl_action,
              payload = std::move(payload)]() mutable {
               if (m_flutter_engine == nullptr) {
                 return;
               }
               const uint8_t* bytes =
                   payload.empty() ? nullptr : payload.data();
               if (LibFlutterEngine->SendSemanticsAction != nullptr) {
                 FlutterSendSemanticsActionInfo info = {};
                 info.struct_size = sizeof(FlutterSendSemanticsActionInfo);
                 info.view_id = view_id;
                 info.node_id = static_cast<uint64_t>(node_id);
                 info.action = fl_action;
                 info.data = bytes;
                 info.data_length = payload.size();
                 LibFlutterEngine->SendSemanticsAction(m_flutter_engine, &info);
                 return;
               }
               // Older engine: the deprecated entry point has no view id, so
               // a multiview dispatch would land on the wrong view. Refuse
               // rather than silently target the implicit one.
               if (view_id != 0) {
                 ihs::log::warn(
                     "Dropping semantics action for view {}: this engine only "
                     "exposes the single-view dispatch entry point",
                     view_id);
                 return;
               }
               LibFlutterEngine->DispatchSemanticsAction(
                   m_flutter_engine, static_cast<uint64_t>(node_id), fl_action,
                   bytes, payload.size());
             });

  // Accepted for delivery. Whether the widget does anything with it is not
  // knowable here, and the hub's contract says so.
  return IHS_SEMANTICS_OK;
}

void Engine::onSemanticsUpdateCallback(const FlutterSemanticsUpdate2* update,
                                       void* user_data) {
  FlutterDesktopEngineState const* engine_state =
      static_cast<FlutterDesktopEngineState*>(user_data);

  auto* accessibility_tree = engine_state->accessibility_tree;
  IHS_TRACE(
      "[onSemanticsUpdateCallback] struct_size: {}, node_count: {} "
      "custom_action_count: {}",
      update->struct_size, update->node_count, update->custom_action_count);

  accessibility_tree->HandleFlutterUpdate(update);

  // Hand the refreshed tree to the semantics hub. Publication is whole-tree
  // and happens here, at the batch boundary, because that is the only point at
  // which the mirror is known consistent.
  const int status = accessibility::PublishTree(*accessibility_tree);
  if (status != IHS_SEMANTICS_OK) {
    ihs::log::warn("Failed to publish semantics snapshot: {}", status);
  }
}
#endif
#if BUILD_WATCHDOG
void Engine::PetWatchdogViaCallback() {
  if (!m_flutter_engine) {
    return;
  }

  LibFlutterEngine->PostCallbackOnAllNativeThreads(
      m_flutter_engine,
      [](FlutterNativeThreadType type, void* /*user_data*/) {
        if (type == kFlutterNativeThreadTypeRender) {
#if BUILD_WATCHDOG
          Watchdog::getInstance().pet(WATCHDOG_SOURCE_RENDER_THREAD);
#endif
        }
      },
      nullptr);
}
#endif