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
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <shell/platform/embedder/embedder.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cassert>

#include "agl-shell-client-protocol.h"
#include "constants.h"
#include "static_plugins/key_event/key_event.h"
#include "static_plugins/text_input/text_input.h"
#include "timer.h"
#include "xdg-shell-client-protocol.h"

#include "configuration/configuration.h"

class Engine;

class Display {
 public:
  explicit Display(bool enable_cursor,
                   std::string cursor_theme_name,
                   const std::vector<Configuration::Config>& configs);

  ~Display();

  Display(const Display&) = delete;

  const Display& operator=(const Display&) = delete;

  std::shared_ptr<EventTimer> m_repeat_timer{};

  /**
   * @brief Get compositor
   * @return wl_compositor*
   * @retval Pointer to compositor
   * @relation
   * wayland
   */
  struct wl_compositor* GetCompositor() {
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
  struct wl_subcompositor* GetSubCompositor() {
    assert(m_subcompositor);
    return m_subcompositor;
  };

  /**
   * @brief Get display
   * @return wl_subcompositor*
   * @retval Pointer to display
   * @relation
   * wayland
   */
  struct wl_display* GetDisplay() {
    assert(m_display);
    return m_display;
  }

  /**
   * @brief Get XDG WM base
   * @return xdg_wm_base*
   * @retval Pointer to XDG WM base
   * @relation
   * wayland
   */
  struct xdg_wm_base* GetXdgWmBase() { return m_xdg_wm_base; }

  /**
   * @brief Get shared memory
   * @return wl_shm*
   * @retval Pointer to shared memory
   * @relation
   * wayland
   */
  struct wl_shm* GetShm() {
    assert(m_shm);
    return m_shm;
  }

  /**
   * @brief Wait for events
   * @return int
   * @retval Number of dispatched events
   * @relation
   * wayland
   */
  int PollEvents();

  /**
   * @brief AglShell: Do background
   * @param[in] surface Image
   * @param[in] index Output index
   * @return void
   * @relation
   * wayland, agl-shell
   */
  void AglShellDoBackground(struct wl_surface*, size_t index);

  /**
   * @brief AglShell: Do panel
   * @param[in] surface Image
   * @param[in] mode Mode
   * @param[in] index Output index
   * @return void
   * @relation
   * wayland, agl-shell
   */
  void AglShellDoPanel(struct wl_surface*,
                       enum agl_shell_edge mode,
                       size_t index);

  /**
   * @brief AglShell: Do ready
   * @return void
   * @relation
   * wayland, agl-shell
   */
  void AglShellDoReady() const;

  /**
   * @brief Set Engine
   * @param[in] surface Image
   * @param[in] engine Engine
   * @return void
   * @relation
   * wayland
   */
  void SetEngine(wl_surface* surface, Engine* engine);

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
  bool ActivateSystemCursor(int32_t device, const std::string& kind);

  /**
   * @brief Set text input
   * @param[in] surface Image
   * @param[in] text_input Pointer of TextInput to set
   * @return void
   * @relation
   * wayland
   */
  void SetTextInput(wl_surface* surface, TextInput* text_input);

  /**
   * @brief Set key event
   * @param[in] surface Image
   * @param[in] text_input Pointer of KeyEvent to set
   * @return void
   * @relation
   * wayland
   */
  void SetKeyEvent(wl_surface* surface, KeyEvent* keyevent);

  wl_output* GetWlOutput(uint32_t index) {
    if (index <= m_all_outputs.size()) {
      return m_all_outputs[index]->output;
    }
    return nullptr;
  }

  int32_t GetBufferScale(uint32_t index);

 private:
  std::shared_ptr<Engine> m_flutter_engine;

  struct wl_display* m_display;
  struct wl_registry* m_registry;
  struct wl_compositor* m_compositor{};
  struct wl_subcompositor* m_subcompositor{};
  struct wl_shm* m_shm{};
  struct wl_surface* m_base_surface{};

  std::map<wl_surface*, Engine*> m_surface_engine_map;
  wl_surface* m_active_surface{};
  Engine* m_active_engine{};
  Engine* m_touch_engine{};

  struct wl_seat* m_seat{};
  struct wl_keyboard* m_keyboard{};

  struct xdg_wm_base* m_xdg_wm_base{};

  struct agl {
    bool bind_to_agl_shell = false;
    struct agl_shell* shell{};

    bool wait_for_bound{};
    bool bound_ok{};
    uint32_t version = 0;
  } m_agl;

  bool m_enable_cursor;
  struct wl_surface* m_cursor_surface{};
  std::string m_cursor_theme_name;

  typedef struct output_info {
    struct wl_output* output;
    uint32_t global_id;
    unsigned width;
    unsigned height;
    MAYBE_UNUSED unsigned physical_width;
    MAYBE_UNUSED unsigned physical_height;
    MAYBE_UNUSED int refresh_rate;
    int32_t scale;
    MAYBE_UNUSED bool done;
  } output_info_t;

  struct pointer_event {
    MAYBE_UNUSED uint32_t event_mask;
    double surface_x, surface_y;
    MAYBE_UNUSED uint32_t button;
    uint32_t state;
    uint32_t time;
    uint32_t serial;
    struct {
      bool valid;
      double value;
      int32_t discrete;
    } axes[2];
    MAYBE_UNUSED uint32_t axis_source;
  };

  struct pointer {
    struct wl_pointer* pointer;
    struct pointer_event event;
    uint32_t serial;

    uint32_t buttons;
    uint32_t state;
  } m_pointer{};

  struct touch_point {
    bool valid;
    int32_t id;
    MAYBE_UNUSED uint32_t event_mask;
    MAYBE_UNUSED wl_fixed_t surface_x;
    wl_fixed_t surface_y;
    wl_fixed_t major, minor;
    MAYBE_UNUSED wl_fixed_t orientation;
  };

  struct touch_event {
    MAYBE_UNUSED uint32_t event_mask;
    uint32_t time;
    uint32_t serial;
  };

  struct touch {
    struct wl_touch* touch;
    struct touch_event event;

    wl_fixed_t surface_x, surface_y;
    uint32_t state;
    FlutterPointerPhase phase;
  } m_touch{};

  // for cursor
  struct wl_cursor_theme* m_cursor_theme{};

  struct xkb_context* m_xkb_context;
  struct xkb_keymap* m_keymap{};
  struct xkb_state* m_xkb_state{};

  xkb_keysym_t m_keysym_pressed{};

  std::map<wl_surface*, TextInput*> m_text_input;
  std::map<wl_surface*, KeyEvent*> m_key_event;

  uint32_t m_repeat_code{};

  std::vector<std::shared_ptr<output_info_t>> m_all_outputs;
  bool m_buffer_scale_enable{};

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

  static const struct wl_output_listener output_listener;

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
                                   int scale);

  /**
   * @brief Turn ON the completion flag
   * @param[in,out] data Data of type output_info_t*
   * @param[in] wl_output No use
   * @return void
   * @relation
   * wayland
   */
  static void display_handle_done(void* data, struct wl_output* wl_output);

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

  static const struct wl_seat_listener seat_listener;

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
  static bool pointerButtonStatePressed(struct pointer* p);

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

  static const struct wl_pointer_listener pointer_listener;

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

  static const struct wl_keyboard_listener keyboard_listener;

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

  static const struct wl_touch_listener touch_listener;

  /**
   * @brief AGL bound ok
   * @param[in,out] data Data of type Display
   * @param[in] shell No use
   * @return void
   * @relation
   * wayland, agl-shell
   * @note Do nothing
   */
  static void agl_shell_bound_ok(void* data, struct agl_shell* shell);

  /**
   * @brief AGL bound fail
   * @param[in,out] data Data of type Display
   * @param[in] shell No use
   * @return void
   * @relation
   * wayland, agl-shell
   * @note Do nothing
   */
  static void agl_shell_bound_fail(void* data, struct agl_shell* shell);

  static const struct agl_shell_listener agl_shell_listener;
};
