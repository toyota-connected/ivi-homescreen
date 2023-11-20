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
#include "platform_channel.h"
#include "textures/texture.h"

#include "hexdump.h"
#include "utils.h"
#include "wayland-client.h"

Engine::Engine(FlutterView* view,
               size_t index,
               const std::vector<const char*>& vm_args_c,
               const std::string& bundle_path,
               int32_t accessibility_features)
    : m_index(index),
      m_running(false),
      m_view(view),
      m_backend(view->GetBackend()),
      m_egl_window(view->GetWindow()),
      m_flutter_engine(nullptr),
      m_cache_path(std::move(GetFilePath(index))),
      m_prev_pixel_ratio(1.0),
      m_accessibility_features(accessibility_features),
      m_prev_width(0),
      m_prev_height(0),
      m_args({
          .struct_size = sizeof(FlutterProjectArgs),
          .assets_path{},
          .icu_data_path{},
          .command_line_argc = static_cast<int>(vm_args_c.size()),
          .command_line_argv = vm_args_c.data(),
          .platform_message_callback =
              [](const FlutterPlatformMessage* message, void* userdata) {
                auto engine = reinterpret_cast<Engine*>(userdata);

                auto platform_channel = PlatformChannel::GetInstance();
                auto callback =
                    platform_channel
                        ->GetHandler()[std::string(message->channel)];

                if (callback == nullptr) {
                  std::stringstream ss;
                  ss << Hexdump(message->message, message->message_size);
                  spdlog::info("Channel: \"{}\"\n{}", message->channel,
                               ss.str());
                  engine->SendPlatformMessageResponse(message->response_handle,
                                                      nullptr, 0);
                } else {
                  callback(message, userdata);
                }
              },
          .persistent_cache_path = m_cache_path.c_str(),
          .is_persistent_cache_read_only = false,
          .log_message_callback =
              [](const char* /* tag */, const char* message, void* user_data) {
#if defined(ENABLE_DART_VM_LOGGING)
                auto engine = reinterpret_cast<Engine*>(user_data);
                std::scoped_lock<std::mutex> lock(engine->m_queue_lock);
                engine->m_vm_queue->emplace(message);
#else
                (void)message;
                (void)user_data;
#endif
              },
      }) {
  SPDLOG_TRACE("({}) +Engine::Engine", m_index);

  /// Task Runner
  m_platform_task_runner =
      std::make_shared<TaskRunner>("Platform", m_proc_table, m_flutter_engine);

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
  } else {
    // override path
    engine_file_path = fml::paths::JoinPaths({bundle_path, kBundleEngine});
    if (fml::IsFile(engine_file_path)) {
      SPDLOG_DEBUG("({}) libflutter_engine.so: {}", m_index, engine_file_path);
    } else {
      engine_file_path = kSystemEngine;
    }
  }
  m_engine_so_handle = dlopen(engine_file_path.c_str(), RTLD_LAZY);
  if (!m_engine_so_handle) {
    spdlog::critical(dlerror());
    exit(-1);
  }
  dlerror();

  FlutterEngineResult (*GetProcAddresses)(FlutterEngineProcTable*);
  GetProcAddresses = (FlutterEngineResult(*)(FlutterEngineProcTable*))dlsym(
      m_engine_so_handle, "FlutterEngineGetProcAddresses");

  char* error = dlerror();
  if (error != nullptr) {
    spdlog::critical(error);
    exit(-1);
  }

  m_proc_table.struct_size = sizeof(FlutterEngineProcTable);
  if (kSuccess != GetProcAddresses(&m_proc_table)) {
    spdlog::critical("FlutterEngineGetProcAddresses != kSuccess");
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
  if (m_proc_table.RunsAOTCompiledDartCode()) {
    m_args.aot_data = nullptr;
    m_aot_data = LoadAotData(fml::paths::JoinPaths({bundle_path, kBundleAot}));
    if (m_aot_data) {
      m_args.aot_data = m_aot_data;
    }
  } else {
    std::stringstream ss_;
    ss_ << "(" << m_index << ") Runtime=debug";
    spdlog::info(ss_.str().c_str());
    ss_.str("");
    ss_.clear();
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
        return reinterpret_cast<Engine*>(context)
            ->m_platform_task_runner->IsThreadEqual(pthread_self());
      },
      .post_task_callback = [](FlutterTask task, uint64_t target_time,
                               void* context) -> void {
        auto e = reinterpret_cast<Engine*>(context);
        e->m_platform_task_runner->QueueFlutterTask(e->m_index, target_time,
                                                    task, context);
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
    m_proc_table.Shutdown(m_flutter_engine);
    if (m_aot_data) {
      m_proc_table.CollectAOTData(m_aot_data);
    }
    dlclose(m_engine_so_handle);
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

FlutterEngineResult Engine::Shutdown() {
  if (!m_flutter_engine) {
    return kSuccess;
  }
  return m_proc_table.Shutdown(m_flutter_engine);
}

bool Engine::IsRunning() const {
  return m_running;
}

FlutterEngineResult Engine::Run(pthread_t event_loop_thread_id) {
  SPDLOG_TRACE("({}) +Engine::Run", m_index);

  auto config = m_backend->GetRenderConfig();
  FlutterEngineResult result = m_proc_table.Initialize(
      FLUTTER_ENGINE_VERSION, &config, &m_args, this, &m_flutter_engine);
  if (result != kSuccess) {
    spdlog::error("({}) FlutterEngineRun failed or engine is null", m_index);
    return result;
  }

  m_event_loop_thread = event_loop_thread_id;

  result = m_proc_table.RunInitialized(m_flutter_engine);
  if (result == kSuccess) {
    m_running = true;
    SPDLOG_DEBUG("({}) Engine::m_running = {}", m_index, m_running);
  }

  // Set available system locales
  FlutterLocale locale = {};
  locale.struct_size = sizeof(locale);
  locale.language_code = kDefaultLocaleLanguageCode;
  locale.country_code = kDefaultLocaleCountryCode;
  locale.script_code = kDefaultLocaleScriptCode;
  locale.variant_code = nullptr;

  std::vector<const FlutterLocale*> locales;
  locales.push_back(&locale);

  if (kSuccess != m_proc_table.UpdateLocales(m_flutter_engine, locales.data(),
                                             locales.size())) {
    spdlog::error("({}) Failed to set Flutter Engine Locale", m_index);
  }

  // Set Accessibility Features
  m_proc_table.UpdateAccessibilityFeatures(
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
  FlutterWindowMetricsEvent fwme = {.struct_size = sizeof(fwme),
                                    .width = width,
                                    .height = height,
                                    .pixel_ratio = m_prev_pixel_ratio};

  auto result = m_proc_table.SendWindowMetricsEvent(m_flutter_engine, &fwme);
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
  FlutterWindowMetricsEvent fwme = {.struct_size = sizeof(fwme),
                                    .width = m_prev_width,
                                    .height = m_prev_height,
                                    .pixel_ratio = pixel_ratio};

  auto result = m_proc_table.SendWindowMetricsEvent(m_flutter_engine, &fwme);
  if (result != kSuccess) {
    spdlog::critical("({}) Failed send initial window size to flutter",
                     m_index);
    assert(false);
  }

  SPDLOG_DEBUG("({}) SetWindowSize: width={}, height={}, pixel_ratio={}",
               m_index, m_prev_width, m_prev_height, pixel_ratio);
  return kSuccess;
}

FlutterEngineResult Engine::TextureRegistryAdd(int64_t texture_id,
                                               Texture* texture) {
  this->m_texture_registry[texture_id] = texture;
  SPDLOG_DEBUG("({}) Added Texture ({}) to registry", m_index, texture_id);
  return kSuccess;
}

MAYBE_UNUSED FlutterEngineResult
Engine::TextureRegistryRemove(int64_t texture_id) {
  auto search =
      std::find_if(m_texture_registry.begin(), m_texture_registry.end(),
                   [&texture_id](const std::pair<int64_t, void*>& element) {
                     return element.first == texture_id;
                   });
  if (search != m_texture_registry.end()) {
    SPDLOG_DEBUG("({}) Removing Texture ({}) from registry {}", m_index,
                 texture_id, static_cast<void*>(search->second));
    m_texture_registry.erase(search);
    spdlog::info("({}) Removed Texture ({}) from registry", m_index,
                 texture_id);

    return kSuccess;
  }
  SPDLOG_DEBUG("({}) Texture Already removed from registry: ({})", m_index,
               texture_id);
  return kInvalidArguments;
}

FlutterEngineResult Engine::TextureEnable(int64_t texture_id) {
  SPDLOG_DEBUG("({}) Enable Texture ID: {}", m_index, texture_id);
  return m_proc_table.RegisterExternalTexture(m_flutter_engine, texture_id);
}

FlutterEngineResult Engine::TextureDisable(int64_t texture_id) {
  SPDLOG_DEBUG("({}) Disable Texture ID: {}", m_index, texture_id);
  return m_proc_table.UnregisterExternalTexture(m_flutter_engine, texture_id);
}

FlutterEngineResult Engine::MarkExternalTextureFrameAvailable(
    const Engine* engine,
    int64_t texture_id) {
  return engine->m_proc_table.MarkExternalTextureFrameAvailable(
      engine->m_flutter_engine, texture_id);
}

flutter::EncodableValue Engine::TextureCreate(
    int64_t texture_id,
    int32_t width,
    int32_t height,
    const std::map<flutter::EncodableValue, flutter::EncodableValue>* args) {
  SPDLOG_DEBUG("({}) Engine::TextureCreate: <{}>", m_index, texture_id);

  auto texture = this->m_texture_registry[texture_id];

  if (texture != nullptr) {
    return texture->Create(width, height, args);
  }

  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("result"), flutter::EncodableValue(-1)},
      {flutter::EncodableValue("error"),
       flutter::EncodableValue("Not found in registry")}});
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

FlutterEngineResult Engine::TextureDispose(int64_t texture_id) {
  SPDLOG_DEBUG("({}) OpenGL Texture: dispose ({})", m_index, texture_id);

  auto search =
      std::find_if(m_texture_registry.begin(), m_texture_registry.end(),
                   [&texture_id](const std::pair<int64_t, void*>& element) {
                     return element.first == texture_id;
                   });
  if (search != m_texture_registry.end()) {
    ((Texture*)search->second)->Dispose(static_cast<uint32_t>(texture_id));
    SPDLOG_DEBUG("({}) Texture Disposed ({})", m_index, texture_id);
    return kSuccess;
  }
  return kInvalidArguments;
}

FlutterEngineResult Engine::SendPlatformMessageResponse(
    const FlutterPlatformMessageResponseHandle* handle,
    const uint8_t* data,
    size_t data_length) const {
  if (!m_running) {
    return kInternalInconsistency;
  }

  return m_proc_table.SendPlatformMessageResponse(m_flutter_engine, handle,
                                                  data, data_length);
}

MAYBE_UNUSED bool Engine::SendPlatformMessage(const char* channel,
                                              const uint8_t* message,
                                              const size_t message_size) const {
  if (!m_running) {
    return kInternalInconsistency;
  }
  const FlutterPlatformMessage msg{
      sizeof(FlutterPlatformMessage), channel, message, message_size, nullptr,
  };
  return (m_proc_table.SendPlatformMessage(m_flutter_engine, &msg) == kSuccess);
}

MAYBE_UNUSED bool Engine::SendPlatformMessage(
    const char* channel,
    const uint8_t* message,
    const size_t message_size,
    const FlutterBinaryReplyUserdata reply,
    void* userdata) const {
  if (!m_running) {
    return kInternalInconsistency;
  }
  FlutterPlatformMessageResponseHandle* handle;
  m_proc_table.PlatformMessageCreateResponseHandle(m_flutter_engine, reply,
                                                   userdata, &handle);
  const FlutterPlatformMessage msg{
      sizeof(FlutterPlatformMessage), channel, message, message_size, handle,
  };

  FlutterEngineResult message_ret =
      m_proc_table.SendPlatformMessage(m_flutter_engine, &msg);
  if (handle != nullptr) {
    m_proc_table.PlatformMessageReleaseResponseHandle(m_flutter_engine, handle);
  }

  return message_ret == kSuccess;
}

MAYBE_UNUSED FlutterEngineResult
Engine::UpdateAccessibilityFeatures(int32_t value) {
  m_accessibility_features = value;
  return m_proc_table.UpdateAccessibilityFeatures(
      m_flutter_engine, static_cast<FlutterAccessibilityFeature>(value));
}

MAYBE_UNUSED FlutterEngineResult
Engine::UpdateLocales(const FlutterLocale** locales, size_t locales_count) {
  return m_proc_table.UpdateLocales(m_flutter_engine, locales, locales_count);
}

void Engine::CoalesceMouseEvent(FlutterPointerSignalKind signal,
                                FlutterPointerPhase phase,
                                double x,
                                double y,
                                double scroll_delta_x,
                                double scroll_delta_y,
                                int64_t buttons) {
  auto timestamp = m_proc_table.GetCurrentTime() / 1000;
  std::scoped_lock lock(m_pointer_mutex);
  m_pointer_events.emplace_back(FlutterPointerEvent {
    .struct_size = sizeof(FlutterPointerEvent), .phase = phase,
#if defined(ENV64BIT)
    .timestamp = timestamp,
#elif defined(ENV32BIT)
    .timestamp = static_cast<size_t>(timestamp & 0xFFFFFFFFULL),
#endif
    .x = x, .y = y, .device = 0, .signal_kind = signal,
    .scroll_delta_x = scroll_delta_x, .scroll_delta_y = scroll_delta_y,
    .device_kind = kFlutterPointerDeviceKindMouse, .buttons = buttons,
    .pan_x = 0, .pan_y = 0, .scale = 0, .rotation = 0
  });
}

void Engine::CoalesceTouchEvent(FlutterPointerPhase phase,
                                double x,
                                double y,
                                int32_t device) {
  auto timestamp = m_proc_table.GetCurrentTime() / 1000;
  std::scoped_lock lock(m_pointer_mutex);
  m_pointer_events.emplace_back(FlutterPointerEvent {
    .struct_size = sizeof(FlutterPointerEvent), .phase = phase,
#if defined(ENV64BIT)
    .timestamp = timestamp,
#elif defined(ENV32BIT)
    .timestamp = static_cast<size_t>(timestamp & 0xFFFFFFFFULL),
#endif
    .x = x, .y = y, .device = device,
    .signal_kind = kFlutterPointerSignalKindNone, .scroll_delta_x = 0.0,
    .scroll_delta_y = 0.0, .device_kind = kFlutterPointerDeviceKindTouch,
    .buttons = 0, .pan_x = 0, .pan_y = 0, .scale = 0, .rotation = 0
  });
}

void Engine::SendPointerEvents() {
  if (!m_pointer_events.empty() && m_flutter_engine) {
    std::scoped_lock lock(m_pointer_mutex);
    m_proc_table.SendPointerEvent(m_flutter_engine, m_pointer_events.data(),
                                  m_pointer_events.size());
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
  auto result = m_proc_table.CreateAOTData(&source, &data);
  if (result != kSuccess) {
    spdlog::critical("({}) Failed to load AOT data from: {}", m_index,
                     aot_data_path);
    return nullptr;
  }
  return data;
}

bool Engine::ActivateSystemCursor(int32_t device, const std::string& kind) {
  return m_egl_window->ActivateSystemCursor(device, kind);
}

#if ENABLE_PLUGIN_TEXT_INPUT
MAYBE_UNUSED void Engine::SetTextInput(TextInput* text_input) {
  m_text_input = text_input;
}

MAYBE_UNUSED TextInput* Engine::GetTextInput() const {
  return m_text_input;
}

#endif

#if ENABLE_PLUGIN_KEY_EVENT
MAYBE_UNUSED void Engine::SetKeyEvent(KeyEvent* key_event) {
  m_key_event = key_event;
}
MAYBE_UNUSED KeyEvent* Engine::GetKeyEvent() const {
  return m_key_event;
}
#endif
