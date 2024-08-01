/*
 * Copyright 2020 Toyota Connected North America
 * @copyright Copyright (c) 2022 Woven Alpha, Inc.
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

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include <flutter/encodable_value.h>
#include <shell/platform/embedder/embedder.h>

#include "backend/backend.h"
#include "config/common.h"
#include "flutter_desktop_engine_state.h"
#include "logging/logging.h"
#include "task_runner.h"
#include "view/flutter_view.h"

class App;
class Backend;
class WaylandWindow;
class FlutterView;
struct FlutterDesktopEngineState;

class Engine {
 public:
  /**
   * @brief Constructor of engine
   * @param[in] view Pointer to Flutter view
   * @param[in] index an index of Flutter view
   * @param[in] vm_args_c Command line arguments
   * @param[in] bundle_path Path to bundle
   * @param[in] accessibility_features Accessibility Features
   * @return Engine
   * @retval Constructed engine class
   * @relation
   * internal
   */
  Engine(FlutterView* view,
         size_t index,
         const std::vector<const char*>& vm_args_c,
         const std::string& bundle_path,
         int32_t accessibility_features);

  ~Engine();

  Engine(const Engine&) = delete;

  const Engine& operator=(const Engine&) = delete;

  MAYBE_UNUSED NODISCARD size_t GetIndex() const { return m_index; }

  /**
   * @brief Run flutter engine
   * @param[in] FlutterDesktopEngineState pointer to struct of engine state
   * @return FlutterEngineResult
   * @retval The result of the run Flutter engine
   * @relation
   * flutter
   */
  FlutterEngineResult Run(FlutterDesktopEngineState* state);

  /**
   * @brief Set window size of flutter
   * @param[in] height Height of flutter window
   * @param[in] width Width of flutter window
   * @return FlutterEngineResult
   * @retval The result of the set window size
   * @relation
   * flutter
   */
  FlutterEngineResult SetWindowSize(size_t height, size_t width);

  /**
   * @brief Set pixel ratio of flutter
   * @param[in] pixel_ratio Pixel ratio of flutter window
   * @return FlutterEngineResult
   * @retval The result of the set pixel ratio
   * @relation
   * flutter
   */
  FlutterEngineResult SetPixelRatio(double pixel_ratio);

  /**
   * @brief Get pixel ratio of flutter
   * @return pixel ratio
   * @retval The result of the previous set pixel ratio
   * @relation
   * flutter
   */
  NODISCARD double GetPixelRatio() const { return m_prev_pixel_ratio; };

  /**
   * @brief Shutsdown Flutter Engine Instance
   * @return FlutterEngineResult
   * @retval The result of shutting down the engine
   * @relation
   * flutter
   */
  NODISCARD FlutterEngineResult Shutdown() const;

  /**
   * @brief Check if engine is running
   * @return bool
   * @retval true If running
   * @retval false If not running
   * @relation
   * internal
   */
  MAYBE_UNUSED NODISCARD bool IsRunning() const;

  /**
   * @brief Run flutter tasks
   * @return FlutterEngineResult
   * @retval The result of the run flutter tasks
   * @relation
   * flutter
   */
  FlutterEngineResult RunTask();

  /**
   * @brief Get persistent cache path
   * @param[in] index Index of path (for log)
   * @return std::string
   * @retval The cache path
   * @relation
   * flutter
   */
  static std::string GetFilePath(size_t index);

  /**
   * @brief Send platform message response
   * @param[in] handle The platform message response handle
   * @param[in] data The data to associate with the platform message response
   * @param[in] data_length The length of the platform message response data
   * @return FlutterEngineResult
   * @retval The result of send platform message resoponse
   * @relation
   * flutter
   */
  FlutterEngineResult SendPlatformMessageResponse(
      const FlutterPlatformMessageResponseHandle* handle,
      const uint8_t* data,
      size_t data_length) const;

  /**
   * @brief Send platform message
   * @param[in] channel Destination channel
   * @param[in] message Message to send
   * @param[in] response_handle optional response handle
   * @return bool
   * @retval true If successed to send message
   * @retval false If failed to send message
   * @relation
   * flutter
   */
  bool SendPlatformMessage(const char* channel,
                           std::unique_ptr<std::vector<uint8_t>> message,
                           const FlutterPlatformMessageResponseHandle*
                               response_handle = nullptr) const;

  /**
   * @brief Send platform message
   * @param[in] channel Destination channel
   * @param[in] message Message to send
   * @param[in] message_size Size of the message
   * @return bool
   * @retval true If successed to send message
   * @retval false If failed to send message
   * @relation
   * flutter
   */
  bool SendPlatformMessage(const char* channel,
                           const uint8_t* message,
                           size_t message_size) const;

  /**
   * @brief Send platform message
   * @param[in] channel Destination channel
   * @param[in] message Message to send
   * @param[in] message_size Size of message
   * @param[in] reply a callback invoked by the engine when the Flutter app send
   * a response on the handle.
   * @param[in] userdata The user data associated with the data callback.
   * @return bool
   * @retval true If successed to send message
   * @retval false If failed to send message
   * @relation
   * flutter
   */
  bool SendPlatformMessage(const char* channel,
                           const uint8_t* message,
                           size_t message_size,
                           FlutterDataCallback reply,
                           void* userdata) const;

  /**
   * @brief Get accessibility features
   * @return int32_t
   * @retval Accessibility features
   * @relation
   * flutter
   */
  MAYBE_UNUSED int32_t GetAccessibilityFeatures() const {
    return m_accessibility_features;
  }

  /**
   * @brief Update accessibility features
   * @param[in] value a value representing accessibility features
   * @return FlutterEngineResult
   * @retval The result of the updating accessibility features
   * @relation
   * flutter
   */
  MAYBE_UNUSED FlutterEngineResult UpdateAccessibilityFeatures(int32_t value);

  /**
   * @brief Update locales
   * @param[in] locales Updated locales in the order of preference
   * @return FlutterEngineResult
   * @retval The result of the updating locales
   * @relation
   * flutter
   */
  MAYBE_UNUSED FlutterEngineResult
  UpdateLocales(std::vector<FlutterLocale> locales);

  /**
   * @brief Get clipboard data
   * @return std::string
   * @retval Clipboard data
   * @relation
   * flutter
   */
  MAYBE_UNUSED std::string GetClipboardData() { return m_clipboard_data; };

  /**
   * @brief Coalesce mouse event
   * @param[in] signal Kind of the signal
   * @param[in] phase Phase of the event
   * @param[in] x X coordinate of the event
   * @param[in] y Y coordinate of the event
   * @param[in] scroll_delta_x X offset of the scroll
   * @param[in] scroll_delta_y Y offset of the scroll
   * @param[in] buttons Buttons currently pressed, if any
   * @return void
   * @relation
   * flutter
   */
  void CoalesceMouseEvent(FlutterPointerSignalKind signal,
                          FlutterPointerPhase phase,
                          double x,
                          double y,
                          double scroll_delta_x,
                          double scroll_delta_y,
                          int64_t buttons);

  /**
   * @brief Coalesce touch event
   * @param[in] phase Phase of the pointer event
   * @param[in] x X coordinate of the pointer event
   * @param[in] y Y coordinate of the pointer event
   * @param[in] device Device identifier
   * @return void
   * @relation
   * flutter
   */
  void CoalesceTouchEvent(FlutterPointerPhase phase,
                          double x,
                          double y,
                          int32_t device);

  /**
   * @brief Send coalesced Pointer events
   * @return void
   * @relation
   * flutter
   */
  void SendPointerEvents();

  /**
   * @brief Activate system cursor
   * @param[in] device No use
   * @param[in] kind Cursor kind
   * @return bool
   * @retval true Normal end
   * @retval false Abnormal end
   * @relation
   * wayland
   */
  NODISCARD bool ActivateSystemCursor(int32_t device,
                                      const std::string& kind) const;

  /**
   * @brief Get backend of view
   * @return Backend*
   * @retval Backend pointer
   * @relation
   * wayland, flutter
   */
  NODISCARD Backend* GetBackend() const { return m_backend; }

  NODISCARD FlutterView* GetView() const { return m_view; }

  static FlutterDesktopMessage ConvertToDesktopMessage(
      const FlutterPlatformMessage& engine_message);

  static void OnFlutterPlatformMessage(
      const FlutterPlatformMessage* engine_message,
      void* user_data);

  static void onLogMessageCallback(const char* tag,
                                   const char* message,
                                   void* user_data);

  FLUTTER_API_SYMBOL(FlutterEngine) GetFlutterEngine() const {
    return m_flutter_engine;
  }

  TaskRunner* GetPlatformTaskRunner() const {
    return m_platform_task_runner.get();
  }

 private:
  size_t m_index;
  bool m_running;

  Backend* m_backend;
  std::shared_ptr<WaylandWindow> m_egl_window;
  FlutterView* m_view;

  std::filesystem::path m_assets_path;
  std::filesystem::path m_icu_data_path;
  std::filesystem::path m_aot_path;
  std::filesystem::path m_cache_path;
  size_t m_prev_height;
  size_t m_prev_width;
  double m_prev_pixel_ratio;
  int32_t m_accessibility_features;

  FLUTTER_API_SYMBOL(FlutterEngine) m_flutter_engine;
  FlutterProjectArgs m_args;
  std::string m_clipboard_data;

  std::shared_ptr<TaskRunner> m_platform_task_runner;
  FlutterTaskRunnerDescription m_platform_task_runner_description{};
  FlutterCustomTaskRunners m_custom_task_runners{};

  FlutterEngineAOTData m_aot_data;

  /**
   * @brief Load AOT data
   * @param[in] aot_data_path Path to AOT data
   * @return FlutterEngineAOTData
   * @retval Loaded AOT data
   * @relation
   * flutter
   */
  MAYBE_UNUSED NODISCARD FlutterEngineAOTData
  LoadAotData(const std::string& bundle_path) const;

  /**
   * @brief Setup Locales
   * @return void
   * @relation
   * flutter
   */
  void SetUpLocales() const;

  std::vector<FlutterPointerEvent> m_pointer_events;
  std::mutex m_pointer_mutex;
};
