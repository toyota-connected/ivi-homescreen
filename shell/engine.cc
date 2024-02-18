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

#include <utility>
#include <vector>

#include <dlfcn.h>
#include <cassert>

#include <flutter/fml/file.h>
#include <flutter/fml/paths.h>

#include "constants.h"
#include "engine.h"
#include "hexdump.h"
#include "utils.h"

extern void EngineOnFlutterPlatformMessage(
    const FlutterPlatformMessage* engine_message,
    void* user_data);

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
      m_cache_path(std::move(GetFilePath(index))),
      m_prev_height(0),
      m_prev_width(0),
      m_prev_pixel_ratio(1.0),
      m_accessibility_features(accessibility_features),
      m_flutter_engine(nullptr),
      m_args({
          .struct_size = sizeof(FlutterProjectArgs),
          .assets_path{},
          .icu_data_path{},
          .command_line_argc = static_cast<int>(vm_args_c.size()),
          .command_line_argv = vm_args_c.data(),
          .platform_message_callback = OnFlutterPlatformMessage,
          .persistent_cache_path = m_cache_path.c_str(),
          .is_persistent_cache_read_only = false,
          .log_message_callback = onLogMessageCallback,
      }) {
  SPDLOG_TRACE("({}) +Engine::Engine", m_index);

  /// Task Runner
  m_platform_task_runner =
      std::make_shared<TaskRunner>("Platform", m_flutter_engine);

  m_vm_queue = std::make_shared<std::queue<std::string>>();

  // Touch events
  m_pointer_events.clear();
  m_pointer_events.reserve(kMaxPointerEvent);

  ///
  /// libflutter_engine.so loading
  ///
  std::string engine_file_path;
  if (bundle_path.empty()) {
    spdlog::critical("Specify bundle folder using --b= option");
    exit(EXIT_FAILURE);
  }

  // override path
  engine_file_path = fml::paths::JoinPaths({bundle_path, kBundleEngine});
  if (fml::IsFile(engine_file_path)) {
    SPDLOG_DEBUG("({}) libflutter_engine.so: {}", m_index, engine_file_path);
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
  m_assets_path = fml::paths::JoinPaths({bundle_path, kBundleFlutterAssets});
  SPDLOG_DEBUG("({}) flutter_assets: {}", m_index, m_assets_path);
  m_args.assets_path = m_assets_path.c_str();

  ///
  /// icudtl.dat file
  ///
  m_icu_data_path = fml::paths::JoinPaths({bundle_path, kBundleIcudtl});
  if (!fml::IsFile(m_icu_data_path)) {
    m_icu_data_path = fml::paths::JoinPaths({kPathPrefix, kSystemIcudtl});
  }
  if (!fml::IsFile(m_icu_data_path)) {
    spdlog::critical("({}) {} is not present.", m_index, m_icu_data_path);
    assert(false);
  }
  SPDLOG_DEBUG("({}) icudtl.dat: {}", m_index, m_icu_data_path);
  m_args.icu_data_path = m_icu_data_path.c_str();

  ///
  /// AOT loading
  ///
  if (LibFlutterEngine->RunsAOTCompiledDartCode()) {
    m_args.aot_data = nullptr;
    m_aot_data = LoadAotData(fml::paths::JoinPaths({bundle_path, kBundleAot}));
    if (m_aot_data) {
      m_args.aot_data = m_aot_data;
    }
  } else {
    spdlog::info("({}) Runtime=debug", m_index);
    std::string kernel_snapshot =
        fml::paths::JoinPaths({m_assets_path, "kernel_blob.bin"});
    if (!fml::IsFile(kernel_snapshot)) {
      spdlog::critical("({}) {} missing Flutter Kernel\0", m_index,
                       kernel_snapshot);
      exit(EXIT_FAILURE);
    }
  }

  /// Configure task runner interop
  m_platform_task_runner_description = {
      .struct_size = sizeof(FlutterTaskRunnerDescription),
      .user_data = this,
      .runs_task_on_current_thread_callback = [](void* context) -> bool {
        const auto engine = static_cast<Engine*>(context);
        return engine->m_platform_task_runner->IsThreadEqual(pthread_self());
      },
      .post_task_callback = [](const FlutterTask task,
                               const uint64_t target_time,
                               void* context) -> void {
        const auto engine = static_cast<Engine*>(context);
        engine->m_platform_task_runner->QueueFlutterTask(
            engine->m_index, target_time, task, context);
      },
  };

  m_custom_task_runners = {
      .struct_size = sizeof(FlutterCustomTaskRunners),
      .platform_task_runner = &m_platform_task_runner_description,
  };

  m_args.custom_task_runners = &m_custom_task_runners;

  SPDLOG_TRACE("({}) -Engine::Engine", m_index);
}

Engine::~Engine() {
  if (m_running) {
    LibFlutterEngine->Deinitialize(m_flutter_engine);
    LibFlutterEngine->Shutdown(m_flutter_engine);
    if (m_aot_data) {
      LibFlutterEngine->CollectAOTData(m_aot_data);
    }
  }
  m_platform_task_runner.reset();
  m_vm_queue.reset();
}

FlutterEngineResult Engine::RunTask() {
  if (!m_flutter_engine) {
    return kSuccess;
  }

  if (!m_vm_queue->empty()) {
    std::scoped_lock<std::mutex> lock(m_queue_lock);
    for (int i = 0; i < kVmLogChunkMax; i++) {
      spdlog::info(m_vm_queue->front());
      m_vm_queue->pop();
      if (m_vm_queue->empty())
        break;
    }
  }

  return kSuccess;
}

FlutterEngineResult Engine::Shutdown() const {
  if (!m_flutter_engine) {
    return kSuccess;
  }
  return LibFlutterEngine->Shutdown(m_flutter_engine);
}

bool Engine::IsRunning() const {
  return m_running;
}

FlutterEngineResult Engine::Run(FlutterDesktopEngineState* state) {
  SPDLOG_TRACE("({}) +Engine::Run", m_index);

  const auto config = m_backend->GetRenderConfig();
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

  SPDLOG_TRACE("({}) -Engine::Run", m_index);
  return result;
}

FlutterEngineResult Engine::SetWindowSize(size_t height, size_t width) {
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
      .pixel_ratio = m_prev_pixel_ratio};

  const auto result =
      LibFlutterEngine->SendWindowMetricsEvent(m_flutter_engine, &fwme);
  if (result != kSuccess) {
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
      .pixel_ratio = pixel_ratio};

  const auto result =
      LibFlutterEngine->SendWindowMetricsEvent(m_flutter_engine, &fwme);
  if (result != kSuccess) {
    spdlog::critical("({}) Failed send initial window size to flutter",
                     m_index);
    assert(false);
  }

  SPDLOG_DEBUG("({}) SetWindowSize: width={}, height={}, pixel_ratio={}",
               m_index, m_prev_width, m_prev_height, pixel_ratio);
  return kSuccess;
}

std::string Engine::GetFilePath(size_t index) {
  auto path = Utils::GetConfigHomePath();

  if (!std::filesystem::is_directory(path) || !std::filesystem::exists(path)) {
    if (!std::filesystem::create_directories(path)) {
      spdlog::critical("({}) create_directories failed: {}", index, path);
      exit(EXIT_FAILURE);
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
    return kInternalInconsistency;
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

MAYBE_UNUSED FlutterEngineResult
Engine::UpdateAccessibilityFeatures(int32_t value) {
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

void Engine::CoalesceMouseEvent(FlutterPointerSignalKind signal,
                                FlutterPointerPhase phase,
                                double x,
                                double y,
                                double scroll_delta_x,
                                double scroll_delta_y,
                                int64_t buttons) {
  auto timestamp = LibFlutterEngine->GetCurrentTime() / 1000;
  std::scoped_lock lock(m_pointer_mutex);
  m_pointer_events.emplace_back(
      FlutterPointerEvent{.struct_size = sizeof(FlutterPointerEvent),
                          .phase = phase,
#if defined(ENV64BIT)
                          .timestamp = timestamp,
#elif defined(ENV32BIT)
                          .timestamp =
                              static_cast<size_t>(timestamp & 0xFFFFFFFFULL),
#endif
                          .x = x,
                          .y = y,
                          .device = 0,
                          .signal_kind = signal,
                          .scroll_delta_x = scroll_delta_x,
                          .scroll_delta_y = scroll_delta_y,
                          .device_kind = kFlutterPointerDeviceKindMouse,
                          .buttons = buttons,
                          .pan_x = 0,
                          .pan_y = 0,
                          .scale = 0,
                          .rotation = 0});
}

void Engine::CoalesceTouchEvent(FlutterPointerPhase phase,
                                double x,
                                double y,
                                int32_t device) {
  auto timestamp = LibFlutterEngine->GetCurrentTime() / 1000;
  std::scoped_lock lock(m_pointer_mutex);
  m_pointer_events.emplace_back(
      FlutterPointerEvent{.struct_size = sizeof(FlutterPointerEvent),
                          .phase = phase,
#if defined(ENV64BIT)
                          .timestamp = timestamp,
#elif defined(ENV32BIT)
                          .timestamp =
                              static_cast<size_t>(timestamp & 0xFFFFFFFFULL),
#endif
                          .x = x,
                          .y = y,
                          .device = device,
                          .signal_kind = kFlutterPointerSignalKindNone,
                          .scroll_delta_x = 0.0,
                          .scroll_delta_y = 0.0,
                          .device_kind = kFlutterPointerDeviceKindTouch,
                          .buttons = 0,
                          .pan_x = 0,
                          .pan_y = 0,
                          .scale = 0,
                          .rotation = 0});
}

void Engine::SendPointerEvents() {
  if (!m_pointer_events.empty() && m_flutter_engine) {
    std::scoped_lock lock(m_pointer_mutex);
    LibFlutterEngine->SendPointerEvent(
        m_flutter_engine, m_pointer_events.data(), m_pointer_events.size());
    m_pointer_events.clear();
  }
}

FlutterEngineAOTData Engine::LoadAotData(
    const std::string& aot_data_path) const {
  if (!fml::IsFile(aot_data_path)) {
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
                     aot_data_path);
    return nullptr;
  }
  return data;
}

bool Engine::ActivateSystemCursor(const int32_t device,
                                  const std::string& kind) const {
  return m_egl_window->ActivateSystemCursor(device, kind);
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

#if defined(DEBUG_PLATFORM_MESSAGES)
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
          spdlog::debug("input_block_cb");
        }
      },
      [view] {
        if (view) {
          spdlog::debug("input_unblock_cb");
        }
      });
}

void Engine::onLogMessageCallback(const char* /* tag */,
                                  const char* message,
                                  void* user_data) {
  (void)message;
  (void)user_data;
}
