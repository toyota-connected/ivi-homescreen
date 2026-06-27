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
#include <utility>
#include <vector>
#include "logging/logging.h"

#include <dlfcn.h>
#include <cassert>

#include "config/common.h"
#include "engine.h"
#include "hexdump.h"
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

// WaylandWindow's complete type is required for the m_egl_window->
// ActivateSystemCursor() call in this TU (and for the shared_ptr
// constructor in the initializer list). flutter_view.h
// forward-declares WaylandWindow unconditionally so the header
// compiles without Wayland; the .cc needs the real definition only
// when Wayland is selected.
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN || \
    BUILD_BACKEND_HEADLESS_EGL
#include "wayland/window.h"
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
               const std::vector<const char*>& vm_args_c,
               const std::string& bundle_path,
               const int32_t accessibility_features)
    : m_index(index),
      m_running(false),
      m_backend(view->GetBackend()),
      m_egl_window(view->GetWindow()),
      m_view(view),
      m_cache_path(GetFilePath(index)),
      m_prev_height(0),
      m_prev_width(0),
      m_prev_pixel_ratio(1.0),
      m_accessibility_features(accessibility_features),
      m_flutter_engine(nullptr) {
  SPDLOG_TRACE("({}) +Engine::Engine", m_index);

  m_args.struct_size = sizeof(FlutterProjectArgs);
  m_args.command_line_argc = static_cast<int>(vm_args_c.size());
  m_args.command_line_argv = vm_args_c.data();
  m_args.dart_entrypoint_argc = static_cast<int>(vm_args_c.size());
  m_args.dart_entrypoint_argv = vm_args_c.data();
  m_args.platform_message_callback = OnFlutterPlatformMessage;
  m_args.persistent_cache_path = m_cache_path.c_str();
  m_args.is_persistent_cache_read_only = false;
  m_args.log_message_callback = onLogMessageCallback;
  m_args.update_semantics_callback2 = onSemanticsUpdateCallback;
  m_args.log_tag = "flutter";

  // Optional per-backend vsync_callback. nullptr (the default for
  // headless / wayland_egl / wayland_vulkan) leaves the field unset
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
    spdlog::critical("Specify bundle folder using --b= option");
    exit(EXIT_FAILURE);
  }

  // override path
  std::filesystem::path engine_file_path(bundle_path);
  engine_file_path /= kBundleEngine;
  if (std::filesystem::exists(engine_file_path)) {
    SPDLOG_DEBUG("({}) libflutter_engine.so: {}", m_index,
                 engine_file_path.c_str());
  } else {
    engine_file_path = kSystemEngine;
  }

  if (!LibFlutterEngine::IsPresent(engine_file_path.c_str())) {
    spdlog::critical(dlerror());
    exit(-1);
  }

  ///
  /// flutter_assets folder
  ///

  m_assets_path = bundle_path;
  m_assets_path /= kBundleFlutterAssets;
  SPDLOG_DEBUG("({}) flutter_assets: {}", m_index, m_assets_path.c_str());
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
    spdlog::critical("({}) {} is not present.", m_index,
                     m_icu_data_path.c_str());
    assert(false);
  }
  SPDLOG_DEBUG("({}) icudtl.dat: {}", m_index, m_icu_data_path.c_str());
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
    spdlog::info("({}) Runtime=debug", m_index);
    std::filesystem::path kernel_snapshot = m_assets_path;
    kernel_snapshot /= "kernel_blob.bin";
    if (!exists(kernel_snapshot)) {
      spdlog::critical("({}) {} missing Flutter Kernel\0", m_index,
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
  // Per-thread priority setter (opt-in via IVI_DRM_RT=1). See
  // EngineThreadPrioritySetter for the mapping rationale.
  m_custom_task_runners.thread_priority_setter = &EngineThreadPrioritySetter;

  m_args.custom_task_runners = &m_custom_task_runners;

  SPDLOG_TRACE("({}) -Engine::Engine", m_index);
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

FlutterEngineResult Engine::RunTask() {
  if (!m_flutter_engine) {
    return kSuccess;
  }

  return kSuccess;
}

void Engine::Shutdown() {
  if (!m_running) {
    return;  // never started, or already stopped — idempotent
  }
  m_running = false;
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

FlutterEngineResult Engine::Run(FlutterDesktopEngineState* state) {
  SPDLOG_TRACE("({}) +Engine::Run", m_index);

  const auto config = m_backend->GetRenderConfig();
  m_compositor = m_backend->GetCompositorConfig();
  if (m_compositor.present_layers_callback != nullptr ||
      m_compositor.create_backing_store_callback != nullptr) {
    m_args.compositor = &m_compositor;
  }
  FlutterEngineResult result = LibFlutterEngine->Initialize(
      FLUTTER_ENGINE_VERSION, &config, &m_args, state, &m_flutter_engine);
  if (result != kSuccess) {
    spdlog::error("({}) FlutterEngineRun failed or engine is null", m_index);
    return result;
  }

  result = LibFlutterEngine->RunInitialized(m_flutter_engine);
  if (result == kSuccess) {
    m_running = true;
    SPDLOG_DEBUG("({}) Engine::m_running = {}", m_index, m_running);
  }

  // Set available system locales
  SetUpLocales();

  // Set Accessibility Features
  LibFlutterEngine->UpdateAccessibilityFeatures(
      m_flutter_engine,
      static_cast<FlutterAccessibilityFeature>(m_accessibility_features));

  // Enable Semantics
  LibFlutterEngine->UpdateSemanticsEnabled(m_flutter_engine, true);

  SPDLOG_TRACE("({}) -Engine::Run", m_index);
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
      .view_id = static_cast<int64_t>(m_index)};

  if (LibFlutterEngine->SendWindowMetricsEvent(m_flutter_engine, &fwme) !=
      kSuccess) {
    spdlog::critical("({}) Failed send initial window size to flutter",
                     m_index);
    assert(false);
  }

  return kSuccess;
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
      .view_id = static_cast<int64_t>(m_index)};

  const auto result =
      LibFlutterEngine->SendWindowMetricsEvent(m_flutter_engine, &fwme);
  if (result != kSuccess) {
    spdlog::critical("({}) Failed send initial window size to flutter",
                     m_index);
    assert(false);
  }

  SPDLOG_TRACE("({}) SetWindowSize: width={}, height={}, pixel_ratio={}",
               m_index, m_prev_width, m_prev_height, pixel_ratio);
  return kSuccess;
}

std::string Engine::GetFilePath(size_t index) {
  auto path = Utils::GetConfigHomePath();

  if (!std::filesystem::is_directory(path) || !std::filesystem::exists(path)) {
    if (!std::filesystem::create_directories(path)) {
      if (!std::filesystem::is_directory(path)) {
        spdlog::critical("({}) create_directories failed: {}", index, path);
        exit(EXIT_FAILURE);
      }
    }
  }

  SPDLOG_DEBUG("({}) PersistentCachePath: {}", index, path);

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
    spdlog::error("Not sending message on Platform Thread");
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
    spdlog::error("({}) Failed to set up Flutter locales.", m_index);
  }
}

void Engine::CoalesceMouseEvent(const FlutterPointerSignalKind signal,
                                const FlutterPointerPhase phase,
                                const double x,
                                const double y,
                                const double scroll_delta_x,
                                const double scroll_delta_y,
                                const int64_t buttons) {
  auto timestamp = LibFlutterEngine->GetCurrentTime() / 1000;
  std::scoped_lock lock(m_pointer_mutex);

  FlutterPointerEvent e{};
  e.struct_size = sizeof(FlutterPointerEvent);
  e.phase = phase;
#if ENV64BIT
  e.timestamp = timestamp;
#elif ENV32BIT
  e.timestamp = static_cast<size_t>(timestamp & 0xFFFFFFFFULL);
#endif
  // Convert surface-local Wayland coords to physical pixels using only the
  // wl_output buffer scale (m_pointer_scale), NOT the full m_prev_pixel_ratio.
  // m_prev_pixel_ratio = config.pixel_ratio * wl_output.scale; the
  // config.pixel_ratio part is purely a Flutter rendering scale and must not
  // be applied to input coordinates — Flutter already divides physical pointer
  // coords by pixel_ratio to reach logical space, so using the full ratio
  // would shift hit targets by the extra config.pixel_ratio factor.
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
  auto timestamp = LibFlutterEngine->GetCurrentTime() / 1000;
  std::scoped_lock lock(m_pointer_mutex);

  FlutterPointerEvent e{};
  e.struct_size = sizeof(FlutterPointerEvent);
  e.phase = phase;
#if ENV64BIT
  e.timestamp = timestamp;
#elif ENV32BIT
  e.timestamp = static_cast<size_t>(timestamp & 0xFFFFFFFFULL);
#endif
  e.x = x * m_pointer_scale;  // see CoalesceMouseEvent for scaling rationale
  e.y = y * m_pointer_scale;
  e.device = device;
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

  // Wake the main loop so it flushes this batch promptly (see above).
  MainLoopWaker::instance().Wake();
}

void Engine::SendPointerEvents() {
  if (!m_pointer_events.empty() && m_flutter_engine) {
    std::scoped_lock lock(m_pointer_mutex);
    LibFlutterEngine->SendPointerEvent(
        m_flutter_engine, m_pointer_events.data(), m_pointer_events.size());
    m_pointer_events.clear();
  }
}

FlutterEngineAOTData Engine::LoadAotData(const std::string& bundle_path) const {
  std::filesystem::path aot_data_path(bundle_path);
  aot_data_path /= kBundleAot;
  if (!exists(aot_data_path)) {
    SPDLOG_DEBUG("({}) AOT file not present", m_index);
    return nullptr;
  }

  spdlog::info("({}) Loading AOT: {}", m_index, aot_data_path.c_str());

  FlutterEngineAOTDataSource source = {};
  source.type = kFlutterEngineAOTDataSourceTypeElfPath;
  source.elf_path = aot_data_path.c_str();

  FlutterEngineAOTData data;
  if (kSuccess != LibFlutterEngine->CreateAOTData(&source, &data)) {
    spdlog::critical("({}) Failed to load AOT data from: {}", m_index,
                     aot_data_path.c_str());
    return nullptr;
  }
  return data;
}

bool Engine::ActivateSystemCursor(const int32_t device,
                                  const std::string& kind) const {
  if (!m_egl_window) {
    return true;
  }
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN || \
    BUILD_BACKEND_HEADLESS_EGL
  return m_egl_window->ActivateSystemCursor(device, kind);
#else
  // Forward-declaration only on non-Wayland builds; m_egl_window is
  // always null here, so this branch is unreachable at runtime. The
  // unused parameter cast keeps the signature stable.
  (void)device;
  (void)kind;
  return true;
#endif
}

void Engine::OnFlutterPlatformMessage(
    const FlutterPlatformMessage* engine_message,
    void* user_data) {
  if (engine_message->struct_size != sizeof(FlutterPlatformMessage)) {
    spdlog::error("Invalid message size received. Expected: {} but received {}",
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
  spdlog::debug("Channel: \"{}\"\n{}", engine_message->channel, ss.str());
#endif

  engine_state->message_dispatcher->HandleMessage(
      {.struct_size = sizeof(FlutterDesktopMessage),
       .channel = engine_message->channel,
       .message = engine_message->message,
       .message_size = engine_message->message_size,
       .response_handle = engine_message->response_handle},
      [view] {
        if (view) {
          SPDLOG_TRACE("input_block_cb");
        }
      },
      [view] {
        if (view) {
          SPDLOG_TRACE("input_unblock_cb");
        }
      });
}

void Engine::onLogMessageCallback(const char* tag,
                                  const char* message,
                                  void* /* user_data */) {
  spdlog::info("{}: {}", tag, message);
}

void Engine::onSemanticsUpdateCallback(const FlutterSemanticsUpdate2* update,
                                       void* user_data) {
  FlutterDesktopEngineState const* engine_state =
      static_cast<FlutterDesktopEngineState*>(user_data);

  auto* accessibility_tree = engine_state->accessibility_tree;
  SPDLOG_TRACE(
      "[onSemanticsUpdateCallback] struct_size: {}, node_count: {} "
      "custom_action_count: {}",
      update->struct_size, update->node_count, update->custom_action_count);

  accessibility_tree->HandleFlutterUpdate(update);
}
