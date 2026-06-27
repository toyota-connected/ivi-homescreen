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
#include <string>
#include <vector>

#include <shell/platform/embedder/embedder.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cassert>

#include "config/common.h"
#include "configuration/configuration.h"
#include "display/idisplay.h"
#include "platform/homescreen/flutter_desktop_view_controller_state.h"
#include "platform/homescreen/key_event_handler.h"
#include "platform/homescreen/keyboard_hook_handler.h"
#include "platform/homescreen/text_input_plugin.h"
#include "timer.h"
#include "vsync/wayland_vsync_provider.h"
#include "wayland/shell/wayland_shell.h"

class Engine;
class WaylandWindow;

struct FlutterDesktopViewControllerState;

class Display : public IDisplay {
 public:
  explicit Display(bool enable_cursor,
                   const std::string& ignore_wayland_event,
                   std::string cursor_theme_name,
                   const std::vector<Configuration::Config>& configs);

  ~Display() override;

  Display(const Display&) = delete;

  const Display& operator=(const Display&) = delete;

  std::shared_ptr<EventTimer> m_repeat_timer{};

  [[nodiscard]] bool HasRepeatTimer() const override {
    // Armed, not merely existing: the repeat timer is created once at keyboard
    // init and lives for the session, so testing existence would keep App::Loop
    // pacing at the refresh rate forever. Only an *armed* timer (a key is held)
    // needs periodic servicing; otherwise the loop may idle.
    return m_repeat_timer && m_repeat_timer->is_armed();
  }

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
   * @brief Get a buffer scale of a specified index of a view
   * @param[in] index Index of a view
   * @return int32_t
   * @retval Buffer scale
   * @relation
   * wayland
   */
  [[nodiscard]] int32_t GetBufferScale(uint32_t index) const override;

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
  std::thread event_thread_;
  std::atomic<bool> stop_events_flag_;
  std::atomic<bool> event_thread_active_;
  std::shared_ptr<Engine> m_flutter_engine;

  struct wl_display* m_display{};
  struct wl_registry* m_registry{};
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
  struct wl_keyboard* m_keyboard{};

  // Compositor-protocol shells (xdg / agl / ivi / simple), built by
  // WaylandShell::Create in priority order. The active one is the first that
  // bound its mandatory global (ActiveShell()). Each shell owns its own
  // protocol state — no per-shell members live on Display anymore.
  std::vector<std::unique_ptr<ivi::WaylandShell>> shells_;

  // wp_presentation vsync provider (owns the wp_presentation global + clock,
  // drives FlutterEngineOnVsync from per-commit feedback).
  ivi::WaylandVsyncProvider m_vsync;

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
    wl_fixed_t surface_x[kMaxTouchFinger];
    wl_fixed_t surface_y[kMaxTouchFinger];
    uint32_t state;
    FlutterPointerPhase phase;
  } m_touch{};

  // for cursor
  struct wl_cursor_theme* m_cursor_theme{};

  struct xkb_context* m_xkb_context;
  struct xkb_keymap* m_keymap{};
  struct xkb_state* m_xkb_state{};

  xkb_keysym_t m_keysym_pressed{};

  // Serialized modifier mask cached in keyboard_handle_modifiers so that
  // keyboard_handle_key and keyboard_repeat_func can read it without calling
  // xkb_state_serialize_mods on every event.
  uint32_t m_mods_effective{};

  std::mutex m_lock;
  uint32_t m_repeat_code{};

  /**
   * @brief set repeat code
   * @param[in] display
   * @param[in] repeat_code a repeat code
   * @return void
   * @relation
   * internal
   */
  static inline void set_repeat_code(Display* display,
                                     const uint32_t repeat_code) {
    std::lock_guard lock(display->m_lock);
    display->m_repeat_code = repeat_code;
  }

  std::vector<std::shared_ptr<output_info_t>> m_all_outputs;

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
  bool m_buffer_scale_enable{};

  // Tracks whether the compositor advertised a GPU buffer-allocation
  // protocol that Mesa's Vulkan WSI can use. Set from
  // registry_handle_global; consumed by the Vulkan backend pre-flight.
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

  static const struct wl_registry_listener registry_listener;

  /**
   * @brief Receive wl_registry events from Wayland server
   * @param[in,out] data Pointer to scatter Display type data
   * @param[in,out] registry Pointer to receive wl_registry event
   * @param[in] name Identifier ID on Wayland server
   * @param[in] interface Wayland interface
   * @param[in] version Interface version
   * @return void
   * @relation
   * wayland
   */
  static void registry_handle_global(void* data,
                                     struct wl_registry* registry,
                                     uint32_t name,
                                     const char* interface,
                                     uint32_t version);

  /**
   * @brief Remove wl_registry events from Wayland server
   * @param[in] data No use
   * @param[in] reg No use
   * @param[in] id No use
   * @return void
   * @relation
   * wayland
   * @note Do nothing
   */
  static void registry_handle_global_remove(void* data,
                                            struct wl_registry* reg,
                                            uint32_t id);

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

  /**
   * @brief Set keymap
   * @param[in,out] data Data of type Display
   * @param[in] keyboard No use
   * @param[in] format No use
   * @param[in] fd File descriptor
   * @param[in] size Mapping region length
   * @return void
   * @relation
   * wayland
   */
  static void keyboard_handle_keymap(void* data,
                                     struct wl_keyboard* keyboard,
                                     uint32_t format,
                                     int fd,
                                     uint32_t size);

  /**
   * @brief Keyboard input event
   * @param[in,out] data Data of type Display
   * @param[in] keyboard No use
   * @param[in] serial No use
   * @param[in] surface Cursor image of keyboard
   * @param[in] keys No use
   * @return void
   * @relation
   * wayland
   */
  static void keyboard_handle_enter(void* data,
                                    struct wl_keyboard* keyboard,
                                    uint32_t serial,
                                    struct wl_surface* surface,
                                    struct wl_array* keys);

  /**
   * @brief Keyboard leaves the surface
   * @param[in,out] data No use
   * @param[in] keyboard No use
   * @param[in] serial No use
   * @param[in] surface No use
   * @return void
   * @relation
   * wayland
   */
  static void keyboard_handle_leave(void* data,
                                    struct wl_keyboard* keyboard,
                                    uint32_t serial,
                                    struct wl_surface* surface);

  /**
   * @brief Key pressed/released
   * @param[in,out] data Data of type Display
   * @param[in] keyboard No use
   * @param[in] serial No use
   * @param[in] time No use
   * @param[in] key Key number
   * @param[in] state Key state released/pressed
   * @return void
   * @relation
   * wayland
   */
  static void keyboard_handle_key(void* data,
                                  struct wl_keyboard* keyboard,
                                  uint32_t serial,
                                  uint32_t time,
                                  uint32_t key,
                                  uint32_t state);

  /**
   * @brief Event when the state of the modifier key changes and lock
   * @param[in,out] data Data of type Display
   * @param[in] keyboard No use
   * @param[in] serial No use
   * @param[in] mods_depressed Flag of modifiers being pushed
   * @param[in] mods_latched Latched modifiers
   * @param[in] mods_locked Locked modifiers
   * @param[in] group Keyboard layout
   * @return void
   * @relation
   * wayland
   */
  static void keyboard_handle_modifiers(void* data,
                                        struct wl_keyboard* keyboard,
                                        uint32_t serial,
                                        uint32_t mods_depressed,
                                        uint32_t mods_latched,
                                        uint32_t mods_locked,
                                        uint32_t group);

  /**
   * @brief Keyboard repeat info
   * @param[in,out] data No use
   * @param[in] wl_keyboard No use
   * @param[in] rate Rate
   * @param[in] delay Delay
   * @return void
   * @relation
   * wayland
   */
  static void keyboard_handle_repeat_info(void* data,
                                          struct wl_keyboard* wl_keyboard,
                                          int32_t rate,
                                          int32_t delay);

  static const wl_keyboard_listener keyboard_listener;

  /**
   * @brief a callback for key repeat behavior
   * @param[in] data Data of type Display
   * @return void
   * @relation
   * wayland
   */
  static void keyboard_repeat_func(void* data);

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

  static const wl_touch_listener touch_listener;
};
