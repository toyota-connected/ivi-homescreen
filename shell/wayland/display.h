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

#include <chrono>
#include <ctime>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <shell/platform/embedder/embedder.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cassert>

#include "asio/io_context.hpp"
#include "asio/posix/stream_descriptor.hpp"

// Core Wayland C++ proxies (wayland::client::CWlKeyboard) and the header-only
// CRTP keyboard handler. wayland_client.hpp (generated from wayland.xml) must
// precede <wl/seat.hpp>, which supplies the wl_seat/wl_keyboard wl_iface()
// definitions and pulls in <wl/keyboard.hpp> (wl::KeyboardHandler). The order
// is load-bearing, so keep clang-format from re-sorting it.
// clang-format off
#include "wayland_client.hpp"
#include <wl/seat.hpp>
#include <wl/client_helpers.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/registry.hpp>
// clang-format on

// Text-input (IME) backend, selected at build time (default text-input-v3).
// <wl/ime/backend.hpp> resolves the WL_IME_BACKEND_* compile define to one
// concrete backend aliased as wl::ime::SelectedTextInput; the v3 backend pulls
// in the generated text_input_v3_client.hpp (emitted by ivi_wayland_protocols)
// by its fixed name. text_input_receiver.hpp supplies the TextInputListener
// contract Display implements.
#include <wl/ime/backend.hpp>
#include <wl/ime/text_input_receiver.hpp>

#include <unordered_map>
#include <utility>

#include "config/common.h"
#include "configuration/configuration.h"
#include "display/idisplay.h"
#include "display/output_provider.h"
#include "engine.h"
#include "platform/homescreen/flutter_desktop_view_controller_state.h"
#include "platform/homescreen/key_event_handler.h"
#include "platform/homescreen/keyboard_hook_handler.h"
#include "platform/homescreen/text_input_plugin.h"
#include "vsync/wayland_vsync_provider.h"
#include "wayland/input_timestamps.h"
#include "wayland/shell/wayland_shell.h"

class Engine;
class WaylandWindow;

struct FlutterDesktopViewControllerState;

class Display : public IDisplay,
                public wl::ime::TextInputListener,
                public homescreen::IOutputProvider {
 public:
  explicit Display(bool enable_cursor,
                   const std::string& ignore_wayland_event,
                   std::string cursor_theme_name,
                   const std::vector<Configuration::Config>& configs);

  ~Display() override;

  Display(const Display&) = delete;

  const Display& operator=(const Display&) = delete;

  // Key repeat is event-driven: wl::KeyboardHandler owns a CLOCK_MONOTONIC
  // timerfd that is registered on the display reactor (repeat_fd_), so the
  // main loop never has to pace it. Always false; the interface keeps the
  // method for the non-Wayland backends whose loop still consults it.
  [[nodiscard]] bool HasRepeatTimer() const override { return false; }

  // The Wayland Display is its own output provider: it already tracks the
  // wl_output globals this enumerates.
  [[nodiscard]] homescreen::IOutputProvider* GetOutputProvider() override {
    return this;
  }

  // homescreen::IOutputProvider.
  [[nodiscard]] std::vector<homescreen::OutputInfo> EnumerateOutputs()
      const override;
  void SetOutputListener(homescreen::IOutputListener* listener) override;
  [[nodiscard]] bool SupportsHotplug() const override { return true; }

  /**
   * @brief Register the shared reactor (App-owned primary io_context) this
   * connection multiplexes its Wayland fd + keyboard repeat timerfd onto. Must
   * be called before StartEvents(). The reactor is single-threaded, so every
   * connection's xkb_state stays single-threaded with no per-connection strand.
   */
  void SetEventLoop(asio::io_context& ioc) { ioc_ = &ioc; }

  /**
   * @brief Key-event sink for wl::KeyboardHandler. Runs on the reactor thread;
   * forwards to KeyCallback (which posts to the engine strand).
   */
  void OnKey(const wl::KeyEvent& ev);

  /**
   * @brief Keymap sink for wl::KeyboardHandler. Posts KeymapChangedCallback
   * onto the engine platform strand with the keymap ref'd.
   */
  void OnKeymap(xkb_keymap* keymap);

  /**
   * @brief Keyboard-focus-leave sink for wl::KeyboardHandler. Posts
   * FocusLostCallback onto the engine platform strand so TextInputPlugin
   * preedit state resets in order with the key-event callbacks. (The handler
   * stops key-repeat internally; this does not touch repeat.)
   */
  void OnKeyboardLeave();

  // ── wl::ime::TextInputListener — events from the compositor's IME ──────────
  // These fire on the reactor thread (Wayland dispatch). Each posts onto the
  // engine platform strand and drives the TextInputPlugin, mirroring the
  // OnKeyboardLeave post pattern so IME edits serialize with the key path.
  void OnEnter() override;
  void OnLeave() override;
  void OnCommitString(std::string_view utf8) override;
  void OnPreeditString(std::string_view utf8,
                       int32_t cursor_begin,
                       int32_t cursor_end) override;
  void OnDeleteSurroundingText(uint32_t before_bytes,
                               uint32_t after_bytes) override;

  // ── Text-input drivers, called FROM the engine platform strand ────────────
  // (TextInputPlugin -> Display). Each posts onto the reactor io_context so the
  // text_input_ proxy is only ever touched on the reactor thread. All no-op
  // gracefully when zwp_text_input_manager_v3 was never advertised (the backend
  // guards on its proxy being bound).

  /**
   * @brief Enable text-input for the focused field: set content type + cursor
   * rectangle, then enable + commit. @p hint / @p purpose are the
   * wl::ime::ContentHint / ContentPurpose values (passed as their underlying
   * integer so the engine side needs no Wayland headers).
   */
  void ActivateTextInput(uint32_t hint,
                         uint32_t purpose,
                         int32_t x,
                         int32_t y,
                         int32_t width,
                         int32_t height);

  /**
   * @brief Disable text-input (field blurred / client cleared) + commit.
   */
  void DeactivateTextInput();

  /**
   * @brief Update the IME candidate-window anchor rectangle + commit.
   */
  void UpdateTextInputCursorRect(int32_t x,
                                 int32_t y,
                                 int32_t width,
                                 int32_t height);

  /**
   * @brief Push the surrounding text + cursor/anchor to the IME + commit.
   */
  void UpdateTextInputSurrounding(const std::string& utf8,
                                  uint32_t cursor,
                                  uint32_t anchor);

  /**
   * @brief Get compositor
   * @return wl_compositor*
   * @retval Pointer to compositor
   * @relation
   * wayland
   */
  [[nodiscard]] wl_compositor* GetCompositor() const {
    assert(m_compositor);
    return m_compositor;
  };

  /**
   * @brief Get sub compositor
   * @return wl_subcompositor*
   * @retval Pointer to sub compositor
   * @relation
   * wayland
   */
  [[nodiscard]] wl_subcompositor* GetSubCompositor() const {
    assert(m_subcompositor);
    return m_subcompositor;
  };

  /**
   * @brief Get display
   * @return wl_display*
   * @retval Pointer to display
   * @relation
   * wayland
   */
  [[nodiscard]] wl_display* GetDisplay() const {
    assert(m_display);
    return m_display;
  }

  /**
   * @brief The active compositor-protocol shell (xdg / agl / ivi / simple) —
   * the first one that bound its mandatory global. WaylandWindow uses it to
   * assign each surface its role.
   */
  [[nodiscard]] ivi::WaylandShell& ActiveShell() const;

  /**
   * @brief Get shared memory
   * @return wl_shm*
   * @retval Pointer to shared memory
   * @relation
   * wayland
   */
  [[nodiscard]] wl_shm* GetShm() const {
    assert(m_shm);
    return m_shm;
  }

  /**
   * @brief The wp_presentation vsync provider. Owns the wp_presentation global
   * (bound during registry enumeration) and drives FlutterEngineOnVsync from
   * per-commit feedback. The Wayland backends route their vsync through it.
   * Always non-null; the provider's Usable() reports whether the compositor
   * actually advertised wp_presentation.
   */
  [[nodiscard]] ivi::WaylandVsyncProvider* GetVsyncProvider() {
    return &m_vsync;
  }

  /**
   * @brief Wait for events
   * @return int
   * @retval Number of dispatched events
   * @relation
   * wayland
   */
  [[nodiscard]] int PollEvents() const override;

  /**
   * @brief Start wayland event thread
   * @return void
   * @relation
   * wayland
   */
  void StartEvents() override;

  /**
   * @brief Stop wayland event thread
   * @return void
   * @relation
   * wayland
   */
  void StopEvents() override;

  // AGL window-management (set_background / set_panel / activation region) now
  // lives on AglShell, reached via ActiveShell().WindowManager().

  /**
   * @brief Set Engine
   * @param[in] surface Image
   * @param[in] engine Engine
   * @return void
   * @relation
   * wayland
   */
  void SetEngine(wl_surface* surface, Engine* engine);

  // Track WaylandWindows so wl_output.mode changes after startup can fan
  // out to their geometry-clamp path (Weston re-resizes its nested
  // output without re-sending xdg_toplevel.configure).
  void RegisterWindow(WaylandWindow* window);
  void UnregisterWindow(WaylandWindow* window);

  void SetViewControllerState(
      FlutterDesktopViewControllerState* view_controller_state) override {
    m_view_controller_state = view_controller_state;
  }

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
  [[nodiscard]] bool ActivateSystemCursor(
      int32_t device,
      const std::string& kind) const override;

  /**
   * @brief Get wl_output of a specified index of a view
   * @param[in] index Index of a view
   * @return wl_output*
   * @retval Pointer to wl_output
   * @relation
   * wayland
   */
  [[nodiscard]] wl_output* GetWlOutput(const uint32_t index) const {
    if (index <= m_all_outputs.size()) {
      return m_all_outputs[index]->output;
    }
    return nullptr;
  }

  /**
   * @brief Index in m_all_outputs of the wl_output with the given name
   *
   * Maps a wl_output name resolved by OutputManager::ResolveForView (the
   * IOutputProvider match against [view.output]) back to the numeric index a
   * WaylandWindow stores as m_output_index and GetWlOutput() takes. Returns
   * nullopt when no live output carries that name.
   *
   * @param[in] name wl_output v4 name (OutputInfo::name)
   * @return std::optional<uint32_t>
   * @relation
   * wayland, flutter
   */
  [[nodiscard]] std::optional<uint32_t> WlOutputIndexForName(
      const std::string& name) const {
    for (size_t i = 0; i < m_all_outputs.size(); ++i) {
      if (m_all_outputs[i] && m_all_outputs[i]->name == name) {
        return static_cast<uint32_t>(i);
      }
    }
    return std::nullopt;
  }

  /**
   * @brief Get a buffer scale of a specified index of a view
   * @param[in] index Index of a view
   * @return int32_t
   * @retval Buffer scale
   * @relation
   * wayland
   */
  [[nodiscard]] int32_t GetBufferScale(uint32_t index) const override;

  // wp_viewporter availability. When true, windows scale via
  // wp_viewport.set_destination (fractional-capable); otherwise they fall
  // back to integer wl_surface.set_buffer_scale.
  [[nodiscard]] bool HasViewporter() const { return m_viewporter != nullptr; }

  // Create a wp_viewport for @p surface, or nullptr when the compositor
  // does not advertise wp_viewporter.
  [[nodiscard]] wl_proxy* CreateViewport(struct wl_surface* surface) const;

  // Create a wp_fractional_scale_v1 for @p surface, or nullptr when the
  // compositor does not advertise wp_fractional_scale_manager_v1. The caller
  // installs its own listener.
  [[nodiscard]] wl_proxy* CreateFractionalScale(
      struct wl_surface* surface) const;

  // Index of the output owning @p output, or OutputCount() if unknown.
  [[nodiscard]] size_t GetOutputIndexByHandle(struct wl_output* output) const;

  [[nodiscard]] size_t OutputCount() const { return m_all_outputs.size(); }

  /**
   * @brief Get a video mode size of a specified index of a view
   * @param[in] index Index of a view
   * @return std::pair<int32_t, int32_t>
   * @retval Video mode size
   * @relation
   * wayland
   */
  [[nodiscard]] std::pair<int32_t, int32_t> GetVideoModeSize(
      uint32_t index) const override;

  /**
   * @brief Get refresh rate of a specified index of a view
   * @param[in] index Index of a view
   * @return double
   * @retval Video Refresh Rate in Hz
   * @relation
   * wayland
   */
  [[nodiscard]] double GetRefreshRate(uint32_t index) const override;

  /**
   * @brief Get max refresh rate of all available views
   * @return double
   * @retval Video Refresh Rate in Hz
   * @relation
   * wayland
   */
  [[nodiscard]] double GetMaxRefreshRate() const override;

  // The AGL application stack (activate/deactivate/app-state) moved to AglShell
  // along with the rest of the AGL window-management surface.

 private:
  // The shared reactor, owned by App (the primary io_context). This connection
  // registers its fds onto it; it does not own or run it. Single-threaded, so
  // the keyboard's xkb_state (read in OnKey / DispatchRepeat) needs no locking.
  asio::io_context* ioc_{};
  // Non-owning async descriptors over the Wayland display fd and the keyboard
  // repeat timerfd. Both fds are owned elsewhere (libwayland / the handler), so
  // these must release() — never close() — them on teardown.
  std::optional<asio::posix::stream_descriptor> wl_fd_;
  std::optional<asio::posix::stream_descriptor> repeat_fd_;

  std::shared_ptr<Engine> m_flutter_engine;

  struct wl_display* m_display{};
  // Owns wl_registry; CRegistry::Reset() must run before wl_display_disconnect.
  wl::CRegistry registry_;
  struct wl_compositor* m_compositor{};
  struct wl_subcompositor* m_subcompositor{};
  struct wl_shm* m_shm{};
  struct wl_surface* m_base_surface{};

  std::map<wl_surface*, Engine*> m_surface_engine_map;
  wl_surface* m_active_surface{};
  Engine* m_active_engine{};
  Engine* m_touch_engine{};

  struct FlutterDesktopViewControllerState* m_view_controller_state{};

  struct wl_seat* m_seat{};
  // wl_keyboard wrapped in the scanner's CRTP handler: owns its own xkb
  // context/keymap/state and the repeat timerfd. Bound off the shared wl_seat
  // in seat_handle_capabilities (pointer + touch stay raw).
  wl::WlPtr<wl::KeyboardHandler<Display>> keyboard_;

  // Text-input backend (selected at build time): owns the per-seat
  // zwp_text_input_v3 + manager proxies. Bound after the startup registry
  // roundtrip once m_seat exists; touched only on the reactor thread. A no-op
  // (unbound) when the compositor never advertised the manager global.
  wl::ime::SelectedTextInput text_input_;

  /**
   * @brief Arm the Wayland display fd on the reactor (prepare_read / flush /
   * async_wait, re-arming itself on each readable edge).
   */
  void ArmWaylandRead();

  /**
   * @brief Register the keyboard handler's repeat timerfd on the reactor (after
   * the keyboard is set up). No-op if the timerfd is unavailable.
   */
  void ArmRepeatFd();

  /**
   * @brief (Re-)arm the wait on the repeat timerfd; on fire, drains it and
   * dispatches one repeat KeyEvent on the reactor thread.
   */
  void ArmRepeatWait();

  /**
   * @brief Cancel + release (never close) the repeat timerfd descriptor.
   */
  void ReleaseRepeatFd();

  /**
   * @brief Cancel + release (never close) the Wayland display fd descriptor.
   */
  void ReleaseWaylandFd();

  // Compositor-protocol shells (xdg / agl / ivi / simple), built by
  // WaylandShell::Create in priority order. The active one is the first that
  // bound its mandatory global (ActiveShell()). Each shell owns its own
  // protocol state — no per-shell members live on Display anymore.
  std::vector<std::unique_ptr<ivi::WaylandShell>> shells_;

  // wp_presentation vsync provider (owns the wp_presentation global + clock,
  // drives FlutterEngineOnVsync from per-commit feedback).
  ivi::WaylandVsyncProvider m_vsync;

  // zwp_input_timestamps_v1 provider (owns the manager global + per-device
  // subscriptions; hands nanosecond kernel input timestamps to the
  // pointer/touch handlers as engine-clock microseconds). Runtime-optional:
  // absent global -> Take*() returns 0 -> arrival-time stamping, the
  // historical behavior.
  ivi::InputTimestamps input_timestamps_;

  bool m_enable_cursor;
  struct wl_surface* m_cursor_surface{};
  std::string m_cursor_theme_name;

  struct wayland_event_mask {
    bool pointer;
    bool pointer_axis;
    bool pointer_buttons;
    bool pointer_motion;
    bool keyboard;
    bool touch;
  } m_wayland_event_mask{};

  typedef struct output_info {
    struct wl_output* output;
    uint32_t global_id;
    unsigned width;
    unsigned height;
    unsigned physical_width;
    unsigned physical_height;
    double refresh_rate;
    int32_t scale;
    bool done;
    // True once OnOutputAdded has been emitted for this output, so a later
    // `done` batch reports OnOutputChanged instead.
    bool announced = false;
    int transform;
    std::string name;
    std::string desc;
    // Back-pointer to the owning Display so the output listeners (which
    // receive the output_info_t* as user data) can fan a mode change out
    // to registered WaylandWindows.
    class Display* display;
  } output_info_t;

  struct pointer_event {
    uint32_t event_mask;
    double surface_x, surface_y;
    uint32_t button;
    uint32_t state;
    uint32_t time;
    uint32_t serial;
    struct {
      bool valid;
      double value;
      int32_t discrete;
    } axes[2];
    uint32_t axis_source;
  };

  struct pointer {
    struct wl_pointer* wl_pointer;
    struct pointer_event event;
    uint32_t serial;

    uint32_t buttons;
    uint32_t state;
  } m_pointer{};

  struct touch_point {
    bool valid;
    int32_t id;
    uint32_t event_mask;
    wl_fixed_t surface_x;
    wl_fixed_t surface_y;
    wl_fixed_t major, minor;
    wl_fixed_t orientation;
  };

  struct touch_event {
    uint32_t event_mask;
    uint32_t time;
    uint32_t serial;
  };

  struct touch_ {
    struct wl_touch* touch;
    struct touch_event event;
    // Active contacts: wl_touch id -> last surface position. Replaces the
    // former fixed surface_x/surface_y[kMaxTouchFinger] arrays: compositor
    // assigned touch ids are only guaranteed unique per session, not bounded
    // by kMaxTouchFinger, so indexing an array by id was an out-of-bounds
    // write waiting for an id >= 10 (finger churn on a 10-slot controller,
    // or any >10-slot panel). The map also lets wl_touch.cancel terminate
    // every active contact instead of just device 0.
    std::unordered_map<int32_t, std::pair<wl_fixed_t, wl_fixed_t>> points;
    // Per-contact updates accumulated since the last wl_touch.frame. The
    // frame event is the protocol's atomicity boundary: everything between
    // two frames is one logically-simultaneous hardware scan, so it is
    // handed to the engine as one batch (one lock, one main-loop wake, one
    // FlutterEngineSendPointerEvent group).
    std::vector<Engine::TouchEvent> frame;
    // Shared high-resolution timestamp for the pending frame: the first
    // nonzero zwp_input_timestamps_v1 value taken by a contact event since
    // the last flush (contacts in one frame are logically simultaneous, and
    // one shared stamp keeps the engine's resampler from interpolating skew
    // between fingers). 0 -> no high-res source -> arrival-time stamping.
    uint64_t frame_time_us;
    uint32_t state;
    FlutterPointerPhase phase;
  } m_touch{};

  // for cursor
  struct wl_cursor_theme* m_cursor_theme{};

  std::vector<std::shared_ptr<output_info_t>> m_all_outputs;

  // The output manager's listener for output add/remove/change; nullptr until
  // SetOutputListener wires it. Notified on the reactor thread.
  homescreen::IOutputListener* m_output_listener = nullptr;

  // Registered WaylandWindows (raw, non-owning). Mutated from the wayland
  // event thread (display_handle_mode callback) and the main thread
  // (WaylandWindow ctor/dtor) so guarded by a mutex.
  std::mutex m_windows_lock;
  std::vector<WaylandWindow*> m_windows;

  // Look up the index of an output_info_t in m_all_outputs (the same
  // numeric value WaylandWindow stores as m_output_index). Returns
  // m_all_outputs.size() if not found.
  size_t IndexOfOutput(const output_info_t* oi) const;

  // Called from display_handle_mode after the output_info_t has been
  // updated. Fans the new width/height out to any WaylandWindow whose
  // m_output_index matches; the window decides whether to shrink.
  void NotifyOutputResized(const output_info_t* oi);

  // Called from display_handle_done: fans the (possibly changed) output
  // scale out to registered windows; they early-out if unchanged.
  void NotifyOutputScaleChanged(const output_info_t* oi);

  // Notify the output listener when a wl_output's `done` event closes its
  // event batch: OnOutputAdded the first time, OnOutputChanged after.
  void EmitOutputDone(output_info_t* oi);
  // Handle a wl_registry.global_remove naming a wl_output: notify the listener
  // and drop it from m_all_outputs.
  void HandleGlobalRemove(uint32_t global_name);
  // Backend-agnostic snapshot of a live wl_output.
  static homescreen::OutputInfo ToOutputInfo(const output_info_t& oi);

  bool m_buffer_scale_enable{};

  // Scaling protocol globals; null when the compositor does not advertise
  // them (e.g. Westeros advertises neither, old Weston only wp_viewporter).
  wl_proxy* m_viewporter{};
  wl_proxy* m_fractional_scale_manager{};

  // Tracks whether the compositor advertised a GPU buffer-allocation
  // protocol that Mesa's Vulkan WSI can use. Set from
  // HandleGlobal; consumed by the Vulkan backend pre-flight.
  bool m_has_linux_dmabuf{false};
  bool m_has_wl_drm{false};

 public:
  [[nodiscard]] bool HasLinuxDmabuf() const { return m_has_linux_dmabuf; }
  [[nodiscard]] bool HasWlDrm() const { return m_has_wl_drm; }

 private:
  static void wayland_event_mask_update(
      const std::string& ignore_wayland_events,
      struct wayland_event_mask& mask);

  static void wayland_event_mask_print(struct wayland_event_mask const& mask);

  /**
   * @brief Handle one wl_registry.global advertisement (member form, driven by
   * CRegistry::OnGlobal). Keeps the existing per-global raw binds (compositor /
   * shm / subcompositor / output / seat + their listeners), the dmabuf/wl_drm
   * capability flags, and the vsync + shells TryBindGlobal chain; additionally
   * records the text-input manager global for text_input_.Bind().
   * @param[in,out] reg The owning registry; reg.Get() yields the raw
   * wl_registry* the shells/vsync still bind through.
   * @param[in] name Identifier ID on the Wayland server.
   * @param[in] iface Advertised interface name.
   * @param[in] ver Advertised interface version.
   * @relation
   * wayland
   */
  void HandleGlobal(wl::CRegistry& reg,
                    uint32_t name,
                    std::string_view iface,
                    uint32_t ver);

  static const wl_output_listener output_listener;

  /**
   * @brief Set physical_width and physical_height
   * @param[in,out] data Data of type output_info_t*
   * @param[in] wl_output No use
   * @param[in] x No use
   * @param[in] y No use
   * @param[in] physical_width Width of the display
   * @param[in] physical_height Height of the display
   * @param[in] subpixel No use
   * @param[in] make No use
   * @param[in] model No use
   * @param[in] transform No use
   * @return void
   * @relation
   * wayland
   */
  static void display_handle_geometry(void* data,
                                      struct wl_output* wl_output,
                                      int x,
                                      int y,
                                      int physical_width,
                                      int physical_height,
                                      int subpixel,
                                      const char* make,
                                      const char* model,
                                      int transform);

  /**
   * @brief Set width, height and refresh rate
   * @param[in,out] data Data of type output_info_t*
   * @param[in] wl_output No use
   * @param[in] flags No use
   * @param[in] width Width of the display
   * @param[in] height Height of the display
   * @param[in] refresh Refresh rate
   * @return void
   * @relation
   * wayland
   */
  static void display_handle_mode(void* data,
                                  struct wl_output* wl_output,
                                  uint32_t flags,
                                  int width,
                                  int height,
                                  int refresh);

  /**
   * @brief Set the display scale
   * @param[in,out] data Data of type output_info_t*
   * @param[in] wl_output No use
   * @param[in] scale Display scale
   * @return void
   * @relation
   * wayland
   */
  static void display_handle_scale(void* data,
                                   struct wl_output* wl_output,
                                   int32_t factor);

  /**
   * @brief Turn ON the completion flag
   * @param[in,out] data Data of type output_info_t*
   * @param[in] wl_output No use
   * @return void
   * @relation
   * wayland
   */
  static void display_handle_done(void* data, struct wl_output* wl_output);

  /**
   * @brief Set the display output name
   * @param[in,out] data Data of type output_info_t*
   * @param[in] wl_output No use
   * @param[in] output_name Display name
   * @return void
   * @relation
   * wayland - since @v4 of wl_output
   */
  static void display_handle_name(void* data,
                                  struct wl_output* wl_output,
                                  const char* output_name);

  /**
   * @brief Set the display description
   * @param[in,out] data Data of type output_info_t*
   * @param[in] wl_output No use
   * @param[in] desc_name Display description name
   * @return void
   * @relation
   * wayland - since @v4 of wl_output
   */
  static void display_handle_desc(void* data,
                                  struct wl_output* wl_output,
                                  const char* desc_name);

  static const struct wl_shm_listener shm_listener;

  /**
   * @brief Do nothing
   * @param[in] data No use
   * @param[in] wl_shm No use
   * @param[in] format No use
   * @return void
   * @relation
   * wayland
   * @note Do nothing
   */
  static void shm_format(void* data, struct wl_shm* wl_shm, uint32_t format);

  static const wl_seat_listener seat_listener;

  /**
   * @brief Set SEAT according to capabilities
   * @param[in] data Data of type Display
   * @param[in] seat SEAT
   * @param[in] caps SEAT capabilities
   * @return void
   * @relation
   * wayland
   */
  static void seat_handle_capabilities(void* data,
                                       struct wl_seat* seat,
                                       uint32_t caps);

  /**
   * @brief Showing the name of the SEAT in Wayland
   * @param[in,out] data No use
   * @param[in] seat No use
   * @param[in] name The name of the SEAT in Wayland
   * @return void
   * @relation
   * wayland
   */
  static void seat_handle_name(void* data,
                               struct wl_seat* seat,
                               const char* name);

  /**
   * @brief Check that the button is pressed
   * @param[in] p Pointer
   * @return bool
   * @retval true Button is pressed
   * @retval false Button is released
   * @relation
   * wayland
   */
  static bool pointerButtonStatePressed(struct pointer const* p);

  /**
   * @brief Pointer goes inside a surface
   * @param[in,out] data Data of type Display
   * @param[in] pointer No use
   * @param[in] serial Pointer to serial
   * @param[in] surface Pointer to cursor image
   * @param[in] sx Pointer to x-axis
   * @param[in] sy Pointer to y-axis
   * @return void
   * @relation
   * wayland
   */
  static void pointer_handle_enter(void* data,
                                   struct wl_pointer* pointer,
                                   uint32_t serial,
                                   struct wl_surface* surface,
                                   wl_fixed_t sx,
                                   wl_fixed_t sy);

  /**
   * @brief Pointer leaves the surface
   * @param[in,out] data Data of type Display
   * @param[in] pointer No use
   * @param[in] serial Pointer to serial
   * @param[in] surface No use
   * @return void
   * @relation
   * wayland
   */
  static void pointer_handle_leave(void* data,
                                   struct wl_pointer* pointer,
                                   uint32_t serial,
                                   struct wl_surface* surface);

  /**
   * @brief Pointer moves within a surface
   * @param[in,out] data Data of type Display
   * @param[in] pointer No use
   * @param[in] time No use
   * @param[in] sx Pointer to x-axis
   * @param[in] sy Pointer to y-axis
   * @return void
   * @relation
   * wayland
   */
  static void pointer_handle_motion(void* data,
                                    struct wl_pointer* pointer,
                                    uint32_t time,
                                    wl_fixed_t sx,
                                    wl_fixed_t sy);

  /**
   * @brief Mouse button pressed/released
   * @param[in,out] data Data of type Display
   * @param[in] wl_pointer No use
   * @param[in] serial Pointer to serial
   * @param[in] time No use
   * @param[in] button Pointer to button state
   * @param[in] state Pointer to state
   * @return void
   * @relation
   * wayland
   */
  static void pointer_handle_button(void* data,
                                    struct wl_pointer* wl_pointer,
                                    uint32_t serial,
                                    uint32_t time,
                                    uint32_t button,
                                    uint32_t state);

  /**
   * @brief Mouse event scroll
   * @param[in,out] data Data of type Display
   * @param[in] wl_pointer No use
   * @param[in] time Time
   * @param[in] axis Moved axis
   * @param[in] value Direction and amount of movement
   * @return void
   * @relation
   * wayland
   */
  static void pointer_handle_axis(void* data,
                                  struct wl_pointer* wl_pointer,
                                  uint32_t time,
                                  uint32_t axis,
                                  wl_fixed_t value);

  /**
   * @brief Mouse event frame
   * @param[in,out] data No use
   * @param[in] wl_pointer No use
   * @return void
   * @relation
   * wayland
   * @note Do nothing
   */
  static void pointer_handle_frame(void* data, struct wl_pointer* wl_pointer);

  /**
   * @brief Related to mouse event scroll
   * @param[in,out] data No use
   * @param[in] wl_pointer No use
   * @param[in] axis_source No use
   * @return void
   * @relation
   * wayland
   * @note Do nothing
   */
  static void pointer_handle_axis_source(void* data,
                                         struct wl_pointer* wl_pointer,
                                         uint32_t axis_source);

  /**
   * @brief Related to mouse event scroll
   * @param[in,out] data No use
   * @param[in] wl_pointer No use
   * @param[in] time No use
   * @param[in] axis No use
   * @return void
   * @relation
   * wayland
   * @note Do nothing
   */
  static void pointer_handle_axis_stop(void* data,
                                       struct wl_pointer* wl_pointer,
                                       uint32_t time,
                                       uint32_t axis);

  /**
   * @brief Related to mouse event scroll
   * @param[in,out] data No use
   * @param[in] wl_pointer No use
   * @param[in] axis No use
   * @param[in] discrete No use
   * @return void
   * @relation
   * wayland
   * @note Do nothing
   */
  static void pointer_handle_axis_discrete(void* data,
                                           struct wl_pointer* wl_pointer,
                                           uint32_t axis,
                                           int32_t discrete);

  static const wl_pointer_listener pointer_listener;

  // The keyboard is no longer a raw wl_keyboard listener: wl::KeyboardHandler
  // owns the wl_keyboard events, xkb processing and repeat timerfd, calling
  // back into Display::OnKey / Display::OnKeymap.

  /**
   * @brief Touch event down
   * @param[in,out] data Data of type Display
   * @param[in] wl_touch No use
   * @param[in] serial No use
   * @param[in] time No use
   * @param[in] surface Cursor image of touch
   * @param[in] id Touch event id
   * @param[in] x_w Touch position x
   * @param[in] y_w Touch position y
   * @return void
   * @relation
   * wayland
   */
  static void touch_handle_down(void* data,
                                struct wl_touch* wl_touch,
                                uint32_t serial,
                                uint32_t time,
                                struct wl_surface* surface,
                                int32_t id,
                                wl_fixed_t x_w,
                                wl_fixed_t y_w);

  /**
   * @brief Touch event up
   * @param[in,out] data Data of type Display
   * @param[in] wl_touch No use
   * @param[in] serial No use
   * @param[in] time No use
   * @param[in] id Touch event id
   * @return void
   * @relation
   * wayland
   */
  static void touch_handle_up(void* data,
                              struct wl_touch* wl_touch,
                              uint32_t serial,
                              uint32_t time,
                              int32_t id);

  /**
   * @brief Touch event move
   * @param[in,out] data Data of type Display
   * @param[in] wl_touch No use
   * @param[in] time No use
   * @param[in] id Touch event id
   * @param[in] x_w Touch position x
   * @param[in] y_w Touch position y
   * @return void
   * @relation
   * wayland
   */
  static void touch_handle_motion(void* data,
                                  struct wl_touch* wl_touch,
                                  uint32_t time,
                                  int32_t id,
                                  wl_fixed_t x_w,
                                  wl_fixed_t y_w);

  /**
   * @brief Touch event cancel
   * @param[in,out] data Data of type Display
   * @param[in] wl_touch No use
   * @return void
   * @relation
   * wayland
   */
  static void touch_handle_cancel(void* data, struct wl_touch* wl_touch);

  /**
   * @brief Touch event frame
   * @param[in,out] data No use
   * @param[in] wl_touch No use
   * @return void
   * @relation
   * wayland
   * @note Do nothing
   */
  static void touch_handle_frame(void* data, struct wl_touch* wl_touch);

  /**
   * @brief Hand the touch updates accumulated since the last wl_touch.frame
   * to the active engine as one batch, then reset the accumulator. No-op
   * when nothing is pending.
   * @param[in] d Display instance
   * @return void
   * @relation
   * wayland, flutter
   */
  static void touch_flush_frame(Display* d);

  /**
   * @brief Append one contact update to the pending touch frame, flushing
   * early if the batch would exceed kMaxPointerEvent. wl_touch.frame is the
   * normal flush boundary, but a broken or hostile compositor could stream
   * down/motion without ever sending it; the cap keeps the accumulator from
   * growing unbounded (parity with the drm/software seats).
   * @param[in] d Display instance
   * @param[in] ev Contact update to queue
   * @param[in] ts_us High-resolution event timestamp in microseconds (engine
   * clock domain), or 0 when unavailable. The first nonzero value since the
   * last flush is latched as the frame's shared timestamp.
   * @return void
   * @relation
   * wayland, flutter
   */
  static void touch_push(Display* d,
                         const Engine::TouchEvent& ev,
                         uint64_t ts_us);

  static const wl_touch_listener touch_listener;
};
