// Copyright 2020 Toyota Connected North America
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

#include <chrono>
#include <vector>

#include <dlfcn.h>
#include <linux/input-event-codes.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cassert>

#include <flutter/fml/logging.h>
#include <flutter/fml/paths.h>

#include "app.h"
#include "constants.h"
#include "egl_window.h"
#include "engine.h"
#include "platform_channel.h"
#include "textures/texture.h"

#include "hexdump.h"
#include "wayland-client.h"

using namespace fml;

static bool IsFile(const std::string& path) {
  struct stat buf {};
  if (stat(path.c_str(), &buf) != 0) {
    return false;
  }

  return S_ISREG(buf.st_mode);
}

static const char* const argv[] = {"homescreen", "--observatory-port=41795",
                                   "--disable-service-auth-codes"};

Engine::Engine(App* app, size_t index)
    : m_index(index),
      m_running(false),
      m_egl_window(app->GetEglWindow(index)),
      m_gl_resolver(app->GetGlResolver()),
      m_flutter_engine(nullptr),
      m_platform_channel(PlatformChannel::GetInstance()),
      m_cache_path(std::move(GetPersistentCachePath())),
      m_args({
          .struct_size = sizeof(FlutterProjectArgs),
          .assets_path = nullptr,
          .icu_data_path = nullptr,
          .command_line_argc = 3,
          .command_line_argv = argv,
          .platform_message_callback =
              [](const FlutterPlatformMessage* message, void* userdata) {
                auto engine = reinterpret_cast<Engine*>(userdata);

                // FML_DLOG(INFO) << "Channel: " << message->channel;
                auto handler = engine->m_platform_channel->GetHandler();

                auto callback = handler[std::string(message->channel)];

                if (callback == nullptr) {
                  std::stringstream ss;
                  ss << Hexdump(message->message, message->message_size);
                  FML_DLOG(INFO) << "Channel: \"" << message->channel << "\"\n"
                                 << ss.str();
                  engine->SendPlatformMessageResponse(message->response_handle,
                                                      nullptr, 0);
                } else {
                  callback(message, userdata);
                }
              },
          .persistent_cache_path = m_cache_path.c_str(),
          .is_persistent_cache_read_only = false,
      }),
      m_renderer_config(
          {.type = kOpenGL,
           .open_gl = {
               .struct_size = sizeof(FlutterOpenGLRendererConfig),
               .make_current = [](void* userdata) -> bool {
                 auto e = reinterpret_cast<Engine*>(userdata);
                 return e->m_egl_window->MakeCurrent(e->m_index);
               },
               .clear_current = [](void* userdata) -> bool {
                 auto e = reinterpret_cast<Engine*>(userdata);
                 return e->m_egl_window->ClearCurrent();
               },
               .present = [](void* userdata) -> bool {
                 auto e = reinterpret_cast<Engine*>(userdata);
                 return e->m_egl_window->SwapBuffers(e->m_index);
               },
               .fbo_callback = [](void* userdata) -> uint32_t { return 0; },
               .make_resource_current = [](void* userdata) -> bool {
                 auto e = reinterpret_cast<Engine*>(userdata);
                 return e->m_egl_window->MakeResourceCurrent(e->m_index);
               },
               .fbo_reset_after_present = false,
               .surface_transformation = nullptr,
               .gl_proc_resolver = [](void* userdata,
                                      const char* name) -> void* {
                 auto e = reinterpret_cast<Engine*>(userdata);
                 return e->m_gl_resolver->gl_process_resolver(name);
               },
               .gl_external_texture_frame_callback =
                   [](void* userdata,
                      int64_t texture_id,
                      size_t width,
                      size_t height,
                      FlutterOpenGLTexture* texture_out) -> bool {
                 auto e = reinterpret_cast<Engine*>(userdata);
                 auto texture = e->m_texture_registry[texture_id];
                 if (texture) {
                   texture->GetFlutterOpenGLTexture(texture_out,
                                                    static_cast<int>(width),
                                                    static_cast<int>(height));
                   return true;
                 }
                 return false;
               },
           }}) {
  FML_DLOG(INFO) << "(" << m_index << ") +Engine::Engine";

  m_icu_data_path.assign(
      paths::JoinPaths({kPathPrefix, "share/flutter/icudtl.dat"}));
  m_assets_path.assign(paths::JoinPaths({kPathPrefix, kFlutterAssetPath}));
  std::string kernel_snapshot =
      paths::JoinPaths({m_assets_path, "kernel_blob.bin"});

  if (!IsFile(kernel_snapshot)) {
    FML_LOG(ERROR) << "(" << m_index << ") " << kernel_snapshot
                   << " missing Flutter Kernel";
    exit(1);
  }

  m_args.assets_path = m_assets_path.c_str();
  FML_DLOG(INFO) << "(" << m_index << ") assets_path: " << m_args.assets_path;

  m_args.icu_data_path = m_icu_data_path.c_str();
  if (!IsFile(m_args.icu_data_path)) {
    FML_LOG(ERROR) << "(" << m_index << ") " << m_icu_data_path << " missing";
    assert(false);
  }
  FML_DLOG(INFO) << "(" << m_index << ") icu_data_path: " << m_icu_data_path;

  // Configure AOT
  m_aot_path.assign(paths::JoinPaths({m_assets_path, "libapp.so"}));
  if (FlutterEngineRunsAOTCompiledDartCode()) {
    m_args.aot_data = nullptr;
    m_aot_data = LoadAotData(m_aot_path);
    if (m_aot_data && m_aot_data.get()) {
      m_args.aot_data = m_aot_data.get();
    }
  } else {
    FML_LOG(INFO) << "Engine instance not able to run AOT compiled Dart code";
  }

  // Configure task runner interop
  m_platform_task_runner = {
      .struct_size = sizeof(FlutterTaskRunnerDescription),
      .user_data = this,
      .runs_task_on_current_thread_callback = [](void* context) -> bool {
        return true;
      },
      .post_task_callback = [](FlutterTask task, uint64_t target_time,
                               void* context) -> void {
        auto* e = static_cast<Engine*>(context);
        // FML_DLOG(INFO) << "(" << e->GetIndex() << ") Post Task";
        e->m_taskrunner.push(std::make_pair(target_time, task));
        if (!e->m_running) {
          uint64_t current = FlutterEngineGetCurrentTime();
          if (current >= e->m_taskrunner.top().first) {
            auto item = e->m_taskrunner.top();
            FML_DLOG(INFO) << "(" << e->GetIndex() << ") Running Task";
            if (kSuccess ==
                FlutterEngineRunTask(e->m_flutter_engine, &item.second)) {
              e->m_taskrunner.pop();
            }
          }
        }
      },
  };

  m_custom_task_runners = {
      .struct_size = sizeof(FlutterCustomTaskRunners),
      .platform_task_runner = &m_platform_task_runner,
  };

  m_args.custom_task_runners = &m_custom_task_runners;

  FML_DLOG(INFO) << "(" << m_index << ") -Engine::Engine";
}

FlutterEngineResult Engine::RunTask() {
  if (!m_flutter_engine) {
    return kSuccess;
  }

  // Handles tasks
  if (!m_taskrunner.empty()) {
    uint64_t current = FlutterEngineGetCurrentTime();
    if (current >= m_taskrunner.top().first) {
      auto item = m_taskrunner.top();
      //      FML_DLOG(INFO) << "(" << m_index
      //                     << ") Running Task: " << current - item.first;
      m_taskrunner.pop();
      return FlutterEngineRunTask(m_flutter_engine, &item.second);
    }
  }
  return kSuccess;
}

[[maybe_unused]] const FlutterLocale* Engine::HandleLocale(
    const FlutterLocale** supported_locales,
    size_t number_of_locales) {
  FML_LOG(INFO) << "number of locale: " << number_of_locales;
  for (int i = 0; i < number_of_locales; i++) {
    FML_LOG(INFO) << "language_code: " << supported_locales[i]->language_code;
    FML_LOG(INFO) << "country_code: " << supported_locales[i]->country_code;
    FML_LOG(INFO) << "script_code: " << supported_locales[i]->script_code;
    FML_LOG(INFO) << "variant_code: " << supported_locales[i]->variant_code;
  }
  return nullptr;
}

bool Engine::IsRunning() const {
  return m_running;
}

FlutterEngineResult Engine::Run() {
  FML_DLOG(INFO) << "(" << m_index << ") +Engine::Run";

  FlutterEngineResult result =
      FlutterEngineInitialize(FLUTTER_ENGINE_VERSION, &m_renderer_config,
                              &m_args, this, &m_flutter_engine);
  if (result != kSuccess) {
    FML_DLOG(ERROR) << "(" << m_index
                    << ") FlutterEngineRun failed or engine is null";
    return result;
  }

  result = FlutterEngineRunInitialized(m_flutter_engine);
  if (result == kSuccess) {
    m_running = true;
    FML_DLOG(INFO) << "(" << m_index << ") Engine::m_running = " << m_running;
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

  if (kSuccess != FlutterEngineUpdateLocales(m_flutter_engine, locales.data(),
                                             locales.size())) {
    FML_DLOG(ERROR) << "Failed to set Flutter Engine Locale";
  }

  FML_DLOG(INFO) << "(" << m_index << ") -Engine::Run";
  return result;
}

FlutterEngineResult Engine::SetWindowSize(size_t height, size_t width) {
  if (!m_running) {
    return kInternalInconsistency;
  }

  // Set window size
  FlutterWindowMetricsEvent fwme = {.struct_size = sizeof(fwme),
                                    .width = width,
                                    .height = height,
                                    .pixel_ratio = 1.0};

  auto result = FlutterEngineSendWindowMetricsEvent(m_flutter_engine, &fwme);
  if (result != kSuccess) {
    FML_DLOG(ERROR) << "(" << m_index
                    << ") Failed send initial window size to flutter";
    assert(false);
  }

  FML_DLOG(INFO) << "(" << m_index << ") SetWindowSize: width=" << width
                 << ", height=" << height;

  return kSuccess;
}

FlutterEngineResult Engine::TextureRegistryAdd(int64_t texture_id,
                                               Texture* texture) {
  this->m_texture_registry[texture_id] = texture;
  FML_DLOG(INFO) << "Added Texture (" << texture_id << ") to registry";
  return kSuccess;
}

[[maybe_unused]] FlutterEngineResult Engine::TextureRegistryRemove(
    int64_t texture_id) {
  auto search =
      std::find_if(m_texture_registry.begin(), m_texture_registry.end(),
                   [&texture_id](const std::pair<int64_t, void*>& element) {
                     return element.first == texture_id;
                   });
  if (search != m_texture_registry.end()) {
    FML_DLOG(INFO) << "Removing Texture (" << texture_id << ") from registry "
                   << search->second;
    m_texture_registry.erase(search);
    FML_LOG(INFO) << "Removed Texture (" << texture_id << ") from registry";

    return kSuccess;
  }
  FML_DLOG(INFO) << "Texture Already removed from registry: (" << texture_id
                 << ")";
  return kInvalidArguments;
}

FlutterEngineResult Engine::TextureEnable(int64_t texture_id) {
  FML_DLOG(INFO) << "Enable Texture ID: " << texture_id;
  return FlutterEngineRegisterExternalTexture(m_flutter_engine, texture_id);
}

FlutterEngineResult Engine::TextureDisable(int64_t texture_id) {
  FML_DLOG(INFO) << "Disable Texture ID: " << texture_id;
  return FlutterEngineUnregisterExternalTexture(m_flutter_engine, texture_id);
}

FlutterEngineResult Engine::MarkExternalTextureFrameAvailable(
    const std::shared_ptr<Engine>& engine,
    int64_t texture_id) {
  return FlutterEngineMarkExternalTextureFrameAvailable(
      engine->m_flutter_engine, texture_id);
}

int64_t Engine::TextureCreate(int64_t texture_id,
                              int32_t width,
                              int32_t height) {
  FML_DLOG(INFO) << "Engine::TextureCreate: <" << texture_id << ">";

  auto texture = this->m_texture_registry[texture_id];

  if (texture != nullptr) {
    int64_t id = texture->Create(width, height);
    return id;
  }
  return -1;
}

std::string Engine::GetPersistentCachePath() {
  const char* homedir;
  if ((homedir = getenv("HOME")) == nullptr) {
    homedir = getpwuid(getuid())->pw_dir;
  }

  auto path = paths::JoinPaths({homedir, kEnginePersistentCacheDir});

  auto res = mkdir(path.c_str(), S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
  if (res < 0) {
    if (errno != EEXIST) {
      FML_LOG(ERROR) << "mkdir failed: " << path;
      exit(EXIT_FAILURE);
    }
  }
  FML_DLOG(INFO) << "PersistentCachePath: " << path;

  return path;
}

FlutterEngineResult Engine::TextureDispose(int64_t texture_id) {
  FML_DLOG(INFO) << "OpenGL Texture: dispose (" << texture_id << ")";

  auto search =
      std::find_if(m_texture_registry.begin(), m_texture_registry.end(),
                   [&texture_id](const std::pair<int64_t, void*>& element) {
                     return element.first == texture_id;
                   });
  if (search != m_texture_registry.end()) {
    ((Texture*)search->second)->Dispose();
    FML_DLOG(INFO) << "Texture Disposed (" << texture_id << ")";
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

  return FlutterEngineSendPlatformMessageResponse(m_flutter_engine, handle,
                                                  data, data_length);
}

[[maybe_unused]] bool Engine::SendPlatformMessage(
    const char* channel,
    const uint8_t* message,
    const size_t message_size) const {
  if (!m_running) {
    return kInternalInconsistency;
  }
  FlutterPlatformMessageResponseHandle* handle;
  FlutterPlatformMessageCreateResponseHandle(
      m_flutter_engine, [](const uint8_t* data, size_t size, void* userdata) {},
      nullptr, &handle);
  const FlutterPlatformMessage msg{
      sizeof(FlutterPlatformMessage), channel, message, message_size, handle,
  };
  return FlutterEngineSendPlatformMessage(m_flutter_engine, &msg);
}

[[maybe_unused]] FlutterEngineResult Engine::UpdateLocales(
    const FlutterLocale** locales,
    size_t locales_count) {
  return FlutterEngineUpdateLocales(m_flutter_engine, locales, locales_count);
}

void Engine::SendMouseEvent(FlutterPointerSignalKind signal,
                            FlutterPointerPhase phase,
                            double x,
                            double y,
                            double scroll_delta_x,
                            double scroll_delta_y,
                            uint32_t button) {
  int64_t buttons = 0;
  if (button & BTN_LEFT)
    buttons |= kFlutterPointerButtonMousePrimary;
  else if (button & BTN_RIGHT)
    buttons |= kFlutterPointerButtonMouseSecondary;
  else if (button & BTN_MIDDLE)
    buttons = kFlutterPointerButtonMouseMiddle;

  FlutterPointerEvent msg = {.struct_size = sizeof(FlutterPointerEvent),
                             .phase = phase,
                             .timestamp = FlutterEngineGetCurrentTime(),
                             .x = x,
                             .y = y,
                             .device = 0,
                             .signal_kind = signal,
                             .scroll_delta_x = scroll_delta_x,
                             .scroll_delta_y = scroll_delta_y,
                             .device_kind = kFlutterPointerDeviceKindMouse,
                             .buttons = buttons};

  FlutterEngineSendPointerEvent(m_flutter_engine, &msg, 1);
}

void Engine::SendTouchEvent(FlutterPointerPhase phase,
                            double x,
                            double y,
                            int32_t device) {
  FlutterPointerEvent msg = {.struct_size = sizeof(FlutterPointerEvent),
                             .phase = phase,
                             .timestamp = FlutterEngineGetCurrentTime(),
                             .x = x,
                             .y = y,
                             .device = device,
                             .signal_kind = kFlutterPointerSignalKindNone,
                             .scroll_delta_x = 0.0,
                             .scroll_delta_y = 0.0,
                             .device_kind = kFlutterPointerDeviceKindTouch,
                             .buttons = 0};

  FlutterEngineSendPointerEvent(m_flutter_engine, &msg, 1);
}

Engine::UniqueAotDataPtr Engine::LoadAotData(const std::string& aot_data_path) {
  if (!IsFile(aot_data_path)) {
    FML_DLOG(INFO) << "AOT file not present";
    return nullptr;
  }

  FML_LOG(INFO) << "Loading AOT: " << aot_data_path.c_str();

  FlutterEngineAOTDataSource source = {};
  source.type = kFlutterEngineAOTDataSourceTypeElfPath;
  source.elf_path = aot_data_path.c_str();

  FlutterEngineAOTData data;

  auto result = FlutterEngineCreateAOTData(&source, &data);
  if (result != kSuccess) {
    FML_DLOG(ERROR) << "Failed to load AOT data from: " << aot_data_path
                    << std::endl;
    return nullptr;
  }
  return UniqueAotDataPtr(data);
}
