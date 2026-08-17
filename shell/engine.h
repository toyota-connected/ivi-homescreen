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
#include <string>
#include <vector>

#include <flutter/encodable_value.h>
#include <shell/platform/embedder/embedder.h>

#include "backend/backend.h"
#include "flutter_desktop_engine_state.h"
#include "logging/logging.h"
#if BUILD_ACCESSIBILITY
#include "shell/accessibility/accessibility_tree.h"
#if ENABLE_ACCESSKIT
#include "shell/accessibility/accesskit_consumer.h"
#endif
#endif
#include "task_runner.h"
#include "view/flutter_view.h"

class App;
class Backend;
class FlutterView;
struct FlutterDesktopEngineState;

#if BUILD_ACCESSIBILITY
// Forward-declared rather than including the hub header: engine.h is widely
// included and this is only ever held as a pointer.
extern "C" {
struct IhsSemanticsSource;
}
#endif

class Engine {
 public:
  /**
   * @brief Constructor of engine
   * @param[in] view Pointer to Flutter view
   * @param[in] index an index of Flutter view
   * @param[in] command_line_args_c Engine / Dart VM switches
   *            (FlutterProjectArgs.command_line_argv; argv[0] is the app id)
   * @param[in] dart_entrypoint_args_c Arguments to the Dart entrypoint
   *            main(List<String>) (FlutterProjectArgs.dart_entrypoint_argv)
   * @param[in] bundle_path Path to bundle
   * @param[in] accessibility_features Accessibility Features
   * @return Engine
   * @retval Constructed engine class
   * @relation
   * internal
   */
  Engine(FlutterView* view,
         size_t index,
         std::string view_name,
         const std::vector<const char*>& command_line_args_c,
         const std::vector<const char*>& dart_entrypoint_args_c,
         const std::string& bundle_path,
         int32_t accessibility_features,
         bool merge_render_platform,
         bool enable_mcp,
         std::vector<std::string> mcp_allowed_tools,
         bool mcp_tools_narrowed);

  ~Engine();

  Engine(const Engine&) = delete;

  const Engine& operator=(const Engine&) = delete;

  [[maybe_unused]] [[nodiscard]] size_t GetIndex() const { return m_index; }

  /**
   * @brief Run flutter engine
   * @param[in] state pointer to struct of engine state
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
   * @brief Attach an additional view to this engine, so one engine can drive
   * several outputs. view_id must be unique and non-zero (0 is the
   * implicit view). Asynchronous -- the engine reports completion via an
   * internal callback; the caller must not present to the view until it
   * succeeds. Sized to the target output. Returns an error without calling the
   * engine when the loaded engine library predates the multi-view API.
   * @relation flutter
   */
  FlutterEngineResult AddView(int64_t view_id,
                              size_t width,
                              size_t height,
                              double pixel_ratio,
                              uint64_t display_id);

  /**
   * @brief Detach a previously added view. The implicit view (0) cannot be
   * removed. Asynchronous (internal completion callback).
   * @relation flutter
   */
  FlutterEngineResult RemoveView(int64_t view_id);

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
  [[nodiscard]] double GetPixelRatio() const { return m_prev_pixel_ratio; };

  // wl_output buffer scale applied to incoming pointer/touch coordinates.
  // Wayland delivers surface-local (logical) coords; Flutter expects physical
  // pixels. Only the output scale applies here — the config pixel_ratio part
  // of m_prev_pixel_ratio is a render scale and would skew hit tests.
  void SetPointerScale(const double scale) { m_pointer_scale = scale; }

  /**
   * @brief Stop the Flutter engine and join all of its threads.
   *
   * This is the explicit shutdown point of control: after it returns no
   * engine thread (platform, UI, rasterizer) is alive, so it is safe to tear
   * down the resources those threads touch (GL contexts, render surfaces).
   * Idempotent — a no-op if the engine was never started or is already
   * stopped. ~Engine() calls it as a fallback.
   * @relation
   * flutter
   */
  void Shutdown();

  /**
   * @brief Check if engine is running
   * @return bool
   * @retval true If running
   * @retval false If not running
   * @relation
   * internal
   */
  [[maybe_unused]] [[nodiscard]] bool IsRunning() const;

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
  [[maybe_unused]] [[nodiscard]] int32_t GetAccessibilityFeatures() const {
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
  [[maybe_unused]] FlutterEngineResult UpdateAccessibilityFeatures(
      int32_t value);

  /**
   * @brief Update locales
   * @param[in] locales Updated locales in the order of preference
   * @return FlutterEngineResult
   * @retval The result of the updating locales
   * @relation
   * flutter
   */
  [[maybe_unused]] FlutterEngineResult UpdateLocales(
      std::vector<FlutterLocale> locales);

  /**
   * @brief Get clipboard data
   * @return std::string
   * @retval Clipboard data
   * @relation
   * flutter
   */
  [[maybe_unused]] std::string GetClipboardData() { return m_clipboard_data; };

  /**
   * @brief Coalesce mouse event
   * @param[in] signal Kind of the signal
   * @param[in] phase Phase of the event
   * @param[in] x X coordinate of the event
   * @param[in] y Y coordinate of the event
   * @param[in] scroll_delta_x X offset of the scroll
   * @param[in] scroll_delta_y Y offset of the scroll
   * @param[in] buttons Buttons currently pressed, if any
   * @param[in] timestamp_us Event timestamp in microseconds in the engine
   * clock domain (CLOCK_MONOTONIC), e.g. from zwp_input_timestamps_v1. 0 (the
   * default) stamps the event with the arrival time instead — the historical
   * behavior, and the fallback whenever no high-resolution source is
   * available.
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
                          int64_t buttons,
                          uint64_t timestamp_us = 0);

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
   * @brief One touch contact update within a hardware scan (touch frame)
   */
  struct TouchEvent {
    FlutterPointerPhase phase;
    double x;
    double y;
    int32_t device;
  };

  /**
   * @brief Coalesce one complete touch frame (a group of logically
   * simultaneous per-contact updates, e.g. everything between two
   * wl_touch.frame events or two libinput TOUCH_FRAME markers).
   *
   * The whole group is appended to the pointer queue under a single lock,
   * stamped with a single timestamp (the contacts moved at the same instant),
   * and the main loop is woken once — so a 10-finger update costs one wake
   * and reaches the engine as one FlutterEngineSendPointerEvent batch, i.e.
   * one UI-thread task post instead of ten.
   *
   * @param[in] events First event of the frame
   * @param[in] count Number of events in the frame
   * @param[in] timestamp_us Shared frame timestamp in microseconds in the
   * engine clock domain (CLOCK_MONOTONIC), e.g. from
   * zwp_input_timestamps_v1. 0 (the default) stamps the frame with the
   * arrival time instead — the historical behavior, and the fallback
   * whenever no high-resolution source is available.
   * @return void
   * @relation
   * flutter
   */
  void CoalesceTouchFrame(const TouchEvent* events,
                          size_t count,
                          uint64_t timestamp_us = 0);

  /**
   * @brief Send coalesced Pointer events
   * @return void
   * @relation
   * flutter
   */
  void SendPointerEvents();

  /*
   * Synthesizes a tap at a point in logical coordinates, for the MCP
   * coordinate fallback -- the escape hatch for a control the semantics tree
   * does not describe. Rides the ordinary input path, so it is hit-tested
   * like a real tap rather than invoking a node's handler by id.
   */
  int SendSyntheticTap(double x, double y);

  /**
   * @brief Get backend of view
   * @return Backend*
   * @retval Backend pointer
   * @relation
   * wayland, flutter
   */
  [[nodiscard]] Backend* GetBackend() const { return m_backend; }

  [[nodiscard]] FlutterView* GetView() const { return m_view; }

  static FlutterDesktopMessage ConvertToDesktopMessage(
      const FlutterPlatformMessage& engine_message);

  static void OnFlutterPlatformMessage(
      const FlutterPlatformMessage* engine_message,
      void* user_data);

  static void onLogMessageCallback(const char* tag,
                                   const char* message,
                                   void* user_data);

  // Completion callbacks for the async AddView/RemoveView; invoked on an
  // engine-managed thread. user_data is the Engine* baton.
  static void OnAddViewComplete(const FlutterAddViewResult* result);
  static void OnRemoveViewComplete(const FlutterRemoveViewResult* result);

#if BUILD_ACCESSIBILITY
  static void onSemanticsUpdateCallback(
      const FlutterSemanticsUpdate2* /* semantics update */,
      void* /* user data*/);
#endif

  [[nodiscard]] FLUTTER_API_SYMBOL(FlutterEngine) GetFlutterEngine() const {
    return m_flutter_engine;
  }

#if BUILD_ACCESSIBILITY
  // Installs this engine as the semantics hub's host, so the hub can turn
  // engine semantics on and off with its consumer count and route dispatches
  // onto the platform thread. Called once, at the end of Run().
  void InstallSemanticsHost();

  // Hub dispatch seam: validates what only the engine can, copies the payload,
  // and posts onto the platform thread. Returns an IhsSemanticsStatus.
  int DispatchSemanticsAction(int64_t view_id,
                              int32_t node_id,
                              uint64_t action,
                              const uint8_t* data,
                              size_t data_length);
#endif

  [[nodiscard]] TaskRunner* GetPlatformTaskRunner() const {
    return m_platform_task_runner.get();
  }

  /**
   * @brief Send a key event to the Flutter engine via the embedder API
   * @param[in] event The key event to send
   * @param[in] callback Callback invoked with whether the event was handled
   * @param[in] user_data User data passed to the callback
   * @return FlutterEngineResult
   */
  FlutterEngineResult SendKeyEvent(const FlutterKeyEvent& event,
                                   FlutterKeyEventCallback callback,
                                   void* user_data) const;

  // Take ownership of the engine state struct. FlutterView constructs and
  // populates the FlutterDesktopEngineState (messenger, message_dispatcher,
  // texture_registrar, plugin_registrar, etc.) before constructing the
  // Engine, then hands it off here. Engine::~Engine releases it AFTER
  // FlutterEngineDeinitialize / Shutdown so the engine can safely dispatch
  // its final platform-message callbacks (whose user_data is this
  // engine_state pointer) without the embedder having freed it underneath.
  void TakeEngineState(std::unique_ptr<FlutterDesktopEngineState> state) {
    m_engine_state = std::move(state);
  }

  [[nodiscard]] FlutterDesktopEngineState* GetEngineState() const {
    return m_engine_state.get();
  }

#if BUILD_WATCHDOG
  void PetWatchdogViaCallback();
#endif

 private:
  size_t m_index;

  // The bundle's directory name, kept for the semantics source's name: it is
  // what a person configuring the shell called this application, so it is what
  // an outside reader should see addressing its tree.
  std::string m_bundle_name;
  // Assigned by App: this view's identity, unique among live views.
  std::string m_view_name;

#if BUILD_ACCESSIBILITY
  // This engine's hub registration. Each engine is an independent publisher --
  // its own tree, its own node id space -- so a shared host would mean the
  // last engine to start receiving every engine's actions.
  IhsSemanticsSource* m_semantics_source = nullptr;
#endif
  bool m_running;

  Backend* m_backend;
  FlutterView* m_view;

  std::filesystem::path m_assets_path;
  std::filesystem::path m_icu_data_path;
  std::filesystem::path m_aot_path;
  std::filesystem::path m_cache_path;
  size_t m_prev_height;
  size_t m_prev_width;
  double m_prev_pixel_ratio;
  double m_pointer_scale{1.0};  // see SetPointerScale()
  int32_t m_accessibility_features;

  FLUTTER_API_SYMBOL(FlutterEngine) m_flutter_engine;
  FlutterProjectArgs m_args{};
  // Compositor config must outlive FlutterEngineInitialize — the engine
  // retains a pointer to it via m_args.compositor.
  FlutterCompositor m_compositor{};
  std::string m_clipboard_data;

  // [global] enable_mcp. Held rather than read at teardown so stop pairs with
  // exactly the start that ran, whatever config does afterwards.
  bool m_enable_mcp = false;

  // [global] mcp_allowed_tools, flattened. The bool carries what an optional
  // would: an empty list is a read-only surface and is not the same as no key
  // at all, which is the compiled default.
  std::vector<std::string> m_mcp_allowed_tools;
  bool m_mcp_tools_narrowed = false;

#if BUILD_ACCESSIBILITY && ENABLE_ACCESSKIT
  // Bridges published snapshots to a screen reader. Owned here because its
  // lifetime is the engine's: constructing it registers with the hub, which is
  // what turns engine semantics on, and destroying it unregisters.
  std::unique_ptr<accessibility::AccessKitConsumer> m_accesskit;
#endif

  std::shared_ptr<TaskRunner> m_platform_task_runner;
  FlutterTaskRunnerDescription m_platform_task_runner_description{};
  // Populated + wired only when m_merge_render_platform: a copy of the platform
  // description with the same identifier, so the engine runs raster on the
  // platform thread. Must outlive FlutterEngineRun (custom_task_runners holds
  // the pointer).
  FlutterTaskRunnerDescription m_render_task_runner_description{};
  FlutterCustomTaskRunners m_custom_task_runners{};

  // Run the raster thread on the platform thread (see the ctor / config).
  bool m_merge_render_platform = false;

  // Zero-initialized: only assigned for AOT runs (RunsAOTCompiledDartCode()).
  // On a JIT/debug run LoadAotData() never runs, so this must be null —
  // otherwise the `if (m_aot_data)` guard in Shutdown() reads garbage and
  // CollectAOTData() corrupts the heap.
  FlutterEngineAOTData m_aot_data{};

  // Owned by Engine so it survives FlutterEngineDeinitialize. See
  // TakeEngineState() above. ~Engine resets this between Shutdown and
  // m_platform_task_runner.reset().
  std::unique_ptr<FlutterDesktopEngineState> m_engine_state;

  /**
   * @brief Load AOT data
   * @param bundle_path Path to bundle
   * @return FlutterEngineAOTData
   * @retval Loaded AOT data
   * @relation
   * flutter
   */
  [[maybe_unused]] [[nodiscard]] FlutterEngineAOTData LoadAotData(
      const std::string& bundle_path) const;

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
