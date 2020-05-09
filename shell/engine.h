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
#include <GLES3/gl3.h>
#include <flutter_embedder.h>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "constants.h"
#include "gl_resolver.h"
#include "platform_channel.h"

class App;
class EglWindow;
class GlResolver;
class Texture;

class Engine {
 public:
  Engine(App* app, size_t index);

  ~Engine() {
    if (m_running) {
      FlutterEngineShutdown(m_flutter_engine);
    }
  }

  Engine(const Engine&) = delete;

  const Engine& operator=(const Engine&) = delete;

  [[maybe_unused]] [[nodiscard]] size_t GetIndex() const { return m_index; }

  [[maybe_unused]] FlutterEngineResult Run();
  FlutterEngineResult SetWindowSize(size_t height, size_t width);

  [[nodiscard]] bool IsRunning() const;

  FlutterEngineResult RunTask();

  FlutterEngineResult TextureRegistryAdd(int64_t texture_id, Texture* texture);
  [[maybe_unused]] FlutterEngineResult TextureRegistryRemove(
      int64_t texture_id);

  FlutterEngineResult TextureEnable(int64_t texture_id);
  FlutterEngineResult TextureDisable(int64_t texture_id);
  static FlutterEngineResult MarkExternalTextureFrameAvailable(
      const std::shared_ptr<Engine>& engine,
      int64_t texture_id);

  int64_t TextureCreate(int64_t texture_id, int32_t width, int32_t height);

  FlutterEngineResult TextureDispose(int64_t texture_id);

  static std::string GetPersistentCachePath();

  FlutterEngineResult SendPlatformMessageResponse(
      const FlutterPlatformMessageResponseHandle* handle,
      const uint8_t* data,
      size_t data_length) const;

  [[maybe_unused]] FlutterEngineResult UpdateLocales(
      const FlutterLocale** locales,
      size_t locales_count);

  [[maybe_unused]]
  std::string GetClipboardData() { return m_clipboard_data; };

  void SendMouseEvent(FlutterPointerSignalKind signal,
                      FlutterPointerPhase phase,
                      double x,
                      double y,
                      double scroll_delta_x,
                      double scroll_delta_y,
                      uint32_t button);

  void SendTouchEvent(FlutterPointerPhase phase,
                      double x,
                      double y,
                      int32_t device);

  [[maybe_unused]]
  Texture* GetTextureObj(int64_t texture_id) {
    return m_texture_registry[texture_id];
  }

  [[maybe_unused]]
  std::shared_ptr<GlResolver> GetGlResolver() { return m_gl_resolver; }

 private:
  size_t m_index;
  bool m_running;

  std::shared_ptr<EglWindow> m_egl_window;
  std::shared_ptr<GlResolver> m_gl_resolver;

  std::string m_assets_path;
  std::string m_icu_data_path;
  std::string m_aot_path;

  PlatformChannel* m_platform_channel;
  std::map<int64_t, Texture*> m_texture_registry;

  FlutterEngine m_flutter_engine;
  FlutterProjectArgs m_args;
  FlutterRendererConfig m_renderer_config{};
  std::string m_clipboard_data;

  [[maybe_unused]]
  static const FlutterLocale* HandleLocale(
      const FlutterLocale** supported_locales,
      size_t number_of_locales);

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

  // Custom deleter for FlutterEngineAOTData.
  struct AOTDataDeleter {
    void operator()(FlutterEngineAOTData aot_data) {
      FlutterEngineCollectAOTData(aot_data);
    }
  };
  using UniqueAotDataPtr =
      std::unique_ptr<_FlutterEngineAOTData, AOTDataDeleter>;
  UniqueAotDataPtr m_aot_data;
  static UniqueAotDataPtr LoadAotData(const std::string& aot_data_path);
};
