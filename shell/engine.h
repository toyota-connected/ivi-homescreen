/*
 * Copyright 2020 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <functional>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <flutter_embedder.h>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "backend/backend.h"
#include "constants.h"
#include "platform_channel.h"
#include "static_plugins/text_input/text_input.h"
#include "view/flutter_view.h"

class App;

class Backend;

class WaylandWindow;

class FlutterView;

class Texture;

#if ENABLE_PLUGIN_TEXT_INPUT

class TextInput;

#endif

class Engine {
 public:
  Engine(FlutterView* view,
         size_t index,
         const std::vector<const char*>& vm_args_c,
         const std::string& bundle_path,
         int32_t accessibility_features);

  ~Engine();

  Engine(const Engine&) = delete;

  const Engine& operator=(const Engine&) = delete;

  MAYBE_UNUSED NODISCARD size_t GetIndex() const { return m_index; }

  FlutterEngineResult Run(pthread_t event_loop_thread_id);

  FlutterEngineResult SetWindowSize(size_t height, size_t width);

  FlutterEngineResult SetPixelRatio(double pixel_ratio);

  MAYBE_UNUSED NODISCARD bool IsRunning() const;

  FlutterEngineResult RunTask();

  FlutterEngineResult TextureRegistryAdd(int64_t texture_id, Texture* texture);

  FlutterEngineResult TextureRegistryRemove(int64_t texture_id);

  FlutterEngineResult TextureEnable(int64_t texture_id);

  FlutterEngineResult TextureDisable(int64_t texture_id);

  static FlutterEngineResult MarkExternalTextureFrameAvailable(
      const std::shared_ptr<Engine>& engine,
      int64_t texture_id);

  flutter::EncodableValue TextureCreate(int64_t texture_id,
                                        int32_t width,
                                        int32_t height);

  FlutterEngineResult TextureDispose(int64_t texture_id);

  static std::string GetPersistentCachePath(size_t index);

  FlutterEngineResult SendPlatformMessageResponse(
      const FlutterPlatformMessageResponseHandle* handle,
      const uint8_t* data,
      size_t data_length) const;

  bool SendPlatformMessage(const char* channel,
                           const uint8_t* message,
                           size_t message_size) const;

  MAYBE_UNUSED int32_t GetAccessibilityFeatures() const {
    return m_accessibility_features;
  }

  MAYBE_UNUSED FlutterEngineResult UpdateAccessibilityFeatures(int32_t value);

  MAYBE_UNUSED FlutterEngineResult UpdateLocales(const FlutterLocale** locales,
                                                 size_t locales_count);

  MAYBE_UNUSED std::string GetClipboardData() { return m_clipboard_data; };

  void SendMouseEvent(FlutterPointerSignalKind signal,
                      FlutterPointerPhase phase,
                      double x,
                      double y,
                      double scroll_delta_x,
                      double scroll_delta_y,
                      int64_t buttons);

  void SendTouchEvent(FlutterPointerPhase phase,
                      double x,
                      double y,
                      int32_t device);

  Texture* GetTextureObj(int64_t texture_id) {
    return m_texture_registry[texture_id];
  }

  bool ActivateSystemCursor(int32_t device, const std::string& kind);

  std::string GetAssetDirectory() { return m_assets_path; }

#if ENABLE_PLUGIN_TEXT_INPUT
  TextInput* m_text_input{};

  void SetTextInput(TextInput* text_input);

  MAYBE_UNUSED NODISCARD TextInput* GetTextInput() const;

#endif

  Backend* GetBackend() {
    return m_backend;
  }

 private:
  size_t m_index;
  bool m_running;

  Backend* m_backend;
  std::shared_ptr<WaylandWindow> m_egl_window;

  std::string m_assets_path;
  std::string m_icu_data_path;
  std::string m_aot_path;
  std::string m_cache_path;
  size_t m_prev_height;
  size_t m_prev_width;
  double m_prev_pixel_ratio;
  int32_t m_accessibility_features;

  std::map<int64_t, Texture*> m_texture_registry;

  FlutterEngine m_flutter_engine;
  FlutterProjectArgs m_args;
  std::string m_clipboard_data;
  pthread_t m_event_loop_thread{};
  void* m_engine_so_handle;
  FlutterEngineProcTable m_proc_table{};

  FlutterTaskRunnerDescription m_platform_task_runner{};
  FlutterCustomTaskRunners m_custom_task_runners{};

  class CompareFlutterTask {
   public:
    bool operator()(std::pair<uint64_t, FlutterTask> n1,
                    std::pair<uint64_t, FlutterTask> n2) {
      return n1.first > n2.first;
    }
  };

  std::priority_queue<std::pair<uint64_t, FlutterTask>,
                      std::vector<std::pair<uint64_t, FlutterTask>>,
                      CompareFlutterTask>
      m_taskrunner;

  FlutterEngineAOTData m_aot_data;
  MAYBE_UNUSED NODISCARD FlutterEngineAOTData
  LoadAotData(const std::string& aot_data_path) const;
};
