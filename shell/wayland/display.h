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
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <shell/platform/embedder/embedder.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cassert>

#include "configuration/configuration.h"
#include "constants.h"
#include "platform/homescreen/flutter_desktop_view_controller_state.h"
#include "platform/homescreen/key_event_handler.h"
#include "platform/homescreen/keyboard_hook_handler.h"
#include "platform/homescreen/text_input_plugin.h"
#include "timer.h"

#if defined(ENABLE_AGL_CLIENT)
#include "agl-shell-client-protocol.h"
#endif
#if defined(ENABLE_IVI_SHELL_CLIENT)
#include "ivi-application-client-protocol.h"
#include "ivi-wm-client-protocol.h"
#endif
#if defined(ENABLE_XDG_CLIENT)
#include "xdg-shell-client-protocol.h"
#endif

class Engine;

struct FlutterDesktopViewControllerState;

class Display {
 public:
  explicit Display(bool enable_cursor,
                   const std::string& ignore_wayland_event,
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
  NODISCARD wl_compositor* GetCompositor() const {
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
  NODISCARD wl_subcompositor* GetSubCompositor() const {
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
  NODISCARD wl_display* GetDisplay() const {
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
#if defined(ENABLE_XDG_CLIENT)
  NODISCARD xdg_wm_base* GetXdgWmBase() const {
    assert(m_xdg_wm_base);
    return m_xdg_wm_base;
  }
#endif

  /**
   * @brief Get ivi_application instance
   * @return ivi_application*
   * @retval Pointer to IVI Application
   * @relation
   * ivi-shell
   */
#if defined(ENABLE_IVI_SHELL_CLIENT)
  NODISCARD ivi_application* GetIviApplication() const {
    return m_ivi_shell.application;
  }
#endif

  /**
   * @brief Get shared memory
   * @return wl_shm*
   * @retval Pointer to shared memory
   * @relation
   * wayland
   */
  NODISCARD wl_shm* GetShm() const {
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
  NODISCARD int PollEvents() const;

#if defined(ENABLE_AGL_CLIENT)
  /**
   * @brief AglShell: Do background
   * @param[in] surface Image
   * @param[in] index Output index
   * @return void
   * @relation
   * wayland, agl-shell
   */
  void AglShellDoBackground(wl_surface* surface, size_t index) const;

  /**
   * @brief AglShell: Do panel
   * @param[in] surface Image
   * @param[in] mode Mode
   * @param[in] index Output index
   * @return void
   * @relation
   * wayland, agl-shell
   */
  void AglShellDoPanel(struct wl_surface* surface,
                       enum agl_shell_edge mode,
                       size_t index) const;

  /**
   * @brief AglShell: Do ready
   * @return void
   * @relation
   * wayland, agl-shell
   */
  void AglShellDoReady() const;

  /**
   * @brief AglShell: Set up an activation area where to display the client's
   * window
   * @return void
   * @param[in] x the x position for the activation rectangle
   * @param[in] y the y position for the activation rectangle
   * @param[in] width the width position for the activation rectangle
   * @param[in] height the height position for the activation rectangle
   * @param[in] index the output, as a number
   * @relation
   *
   * see agl-shell::set_activate_region request for more information. The x and
   * y values are the position of an activation rectangle, with the width and
   * height grabbed from the output itself. This would specify the area where
   * the client's window will be displayed.
   *
   * --------------------
   * |                  |
   * |  (x, y)          |
   * |  +--------       |
   * |  |       |       |
   * |  |       | height|
   * |  |       |       |
   * |  ---------       |
   * |    width		|
   * .			|
   * |			|
   * --------------------
   */
  void AglShellDoSetupActivationArea(uint32_t x,
                                     uint32_t y,
                                     uint32_t width,
                                     uint32_t height,
                                     uint32_t index) const;
#endif

  /**
   * @brief Set Engine
   * @param[in] surface Image
   * @param[in] engine Engine
   * @return void
   * @relation
   * wayland
   */
  void SetEngine(wl_surface* surface, Engine* engine);

  void SetViewControllerState(
      FlutterDesktopViewControllerState* view_controller_state) {
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
  NODISCARD bool ActivateSystemCursor(int32_t device,
                                      const std::string& kind) const;

  /**
   * @brief Get wl_output of a specified index of a view
   * @param[in] index Index of a view
   * @return wl_output*
   * @retval Pointer to wl_output
   * @relation
   * wayland
   */
  NODISCARD wl_output* GetWlOutput(const uint32_t index) const {
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
  NODISCARD int32_t GetBufferScale(uint32_t index) const;

  /**
   * @brief Get a video mode size of a specified index of a view
   * @param[in] index Index of a view
   * @return std::pair<int32_t, int32_t>
   * @retval Video mode size
   * @relation
   * wayland
   */
  NODISCARD std::pair<int32_t, int32_t> GetVideoModeSize(uint32_t index) const;

  /**
   * @brief deactivate/hide the application pointed by app_id
   * @param[in] app_id the app_id
   * @relation
   * agl_shell
   */
  void deactivateApp(std::string app_id);
  /**
   * @brief activate/show the application pointed by app_id
   * @param[in] app_id the app_id
   * @relation
   * agl_shell
   */
  void activateApp(std::string app_id);
  /**
   * @brief Add app_id to a list of list applications
   * @param[in] app_id the app_id
   * @relation
   * agl_shell
   */
  void addAppToStack(std::string app_id);
  /**
   * @brief Helper to retrieve the output using its output_name
   * @param[in] output_name a std::string representing the output
   * @retval an integer that can used to get the proper output
   * @relation
   * agl_sell
   */
  int find_output_by_name(std::string output_name);
  /**
   * @brief helper to process the application status
   * @param[in] app_id an array of char
   * @param[in] event_type a std::string representing the type of event
   * (started/stopped/terminated)
   * @relation
   * agl_shell
   */
  void processAppStatusEvent(const char* app_id, const std::string event_type);

 private:
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

  struct xdg_wm_base* m_xdg_wm_base{};

  struct agl {
    bool bind_to_agl_shell = false;
    struct agl_shell* shell{};

    bool wait_for_bound = true;
    bool bound_ok{};
    uint32_t version = 0;
  } m_agl;

  std::list<std::string> apps_stack;
  std::list<std::pair<const std::string, const std::string>> pending_app_list;

  struct ivi_shell {
    struct ivi_application* application = nullptr;
    struct ivi_wm* ivi_wm = nullptr;
  } m_ivi_shell;

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
    MAYBE_UNUSED unsigned physical_width;
    MAYBE_UNUSED unsigned physical_height;
    MAYBE_UNUSED int refresh_rate;
    int32_t scale;
    MAYBE_UNUSED bool done;
    int transform;
    std::string name;
    std::string desc;
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
    struct wl_pointer* wl_pointer;
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
  bool m_buffer_scale_enable{};

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

  /**
   * @brief AGL app_state event
   * @param[in,out] data Data of type Display
   * @param[in] agl_shell No use
   * @param[in] app_id the application id for which this event was sent
   * @param[in] state the state: CREATED/TERMINATED/ACTIVATED/DEACTIVATED
   * @return void
   * @relation
   * wayland, agl-shell
   * @note Do nothing
   */
  static void agl_shell_app_state(void* data,
                                  struct agl_shell* agl_shell,
                                  const char* app_id,
                                  uint32_t state);

  /**
   * @brief AGL app_app_on_output event
   * @param[in,out] data Data of type Display
   * @param[in] shell No use
   * @param[in] app_id the application id for which this event was sent
   * @param[in] state the state: CREATED/TERMINATED/ACTIVATED/DEACTIVATED
   * @return void
   * @relation
   * wayland, agl-shell
   * @note Do nothing
   */
  static void agl_shell_app_on_output(void* data,
                                      struct agl_shell* agl_shell,
                                      const char* app_id,
                                      const char* output_name);

  static const struct agl_shell_listener agl_shell_listener;

  /**
   * @brief handler of a visibility of a ivi shell surface
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] surface_id surface id
   * @param[in] visibility visibility
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_surface_visibility(void* data,
                                        struct ivi_wm* ivi_wm,
                                        uint32_t surface_id,
                                        int32_t visibility);
  /**
   * @brief handler of a visibility of a ivi shell layer
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] layer_id layer id
   * @param[in] visibility visibility
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_layer_visibility(void* data,
                                      struct ivi_wm* ivi_wm,
                                      uint32_t layer_id,
                                      int32_t visibility);
  /**
   * @brief handler of an opacity of a ivi shell surface
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] surface_id surface id
   * @param[in] opacity opacity
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_surface_opacity(void* data,
                                     struct ivi_wm* ivi_wm,
                                     uint32_t surface_id,
                                     wl_fixed_t opacity);
  /**
   * @brief handler of an opacity of a ivi shell layer
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] layer_id layer id
   * @param[in] opacity opacity
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_layer_opacity(void* data,
                                   struct ivi_wm* ivi_wm,
                                   uint32_t layer_id,
                                   wl_fixed_t opacity);
  /**
   * @brief handler of a source rectangle of a ivi shell surface
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] surface_id surface id
   * @param[in] x x position of source rectangle
   * @param[in] y y position of source rectangle
   * @param[in] width width of source rectangle
   * @param[in] height height of source rectangle
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_surface_source_rectangle(void* data,
                                              struct ivi_wm* ivi_wm,
                                              uint32_t surface_id,
                                              int32_t x,
                                              int32_t y,
                                              int32_t width,
                                              int32_t height);
  /**
   * @brief handler of a source rectangle of a ivi shell layer
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] layer_id layer id
   * @param[in] x x position of source rectangle
   * @param[in] y y position of source rectangle
   * @param[in] width width of source rectangle
   * @param[in] height height of source rectangle
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_layer_source_rectangle(void* data,
                                            struct ivi_wm* ivi_wm,
                                            uint32_t layer_id,
                                            int32_t x,
                                            int32_t y,
                                            int32_t width,
                                            int32_t height);
  /**
   * @brief handler of a destination rectangle of a ivi shell surface
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] surface_id surface id
   * @param[in] x x position of destination rectangle
   * @param[in] y y position of destination rectangle
   * @param[in] width width of destination rectangle
   * @param[in] height height of destination rectangle
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_surface_destination_rectangle(void* data,
                                                   struct ivi_wm* ivi_wm,
                                                   uint32_t surface_id,
                                                   int32_t x,
                                                   int32_t y,
                                                   int32_t width,
                                                   int32_t height);
  /**
   * @brief handler of a destination rectangle of a ivi shell layer
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] layer_id layer id
   * @param[in] x x position of destination rectangle
   * @param[in] y y position of destination rectangle
   * @param[in] width width of destination rectangle
   * @param[in] height height of destination rectangle
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_layer_destination_rectangle(void* data,
                                                 struct ivi_wm* ivi_wm,
                                                 uint32_t layer_id,
                                                 int32_t x,
                                                 int32_t y,
                                                 int32_t width,
                                                 int32_t height);
  /**
   * @brief handler for created event of a ivi shell surface
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] surface_id surface id
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_surface_created(void* data,
                                     struct ivi_wm* ivi_wm,
                                     uint32_t surface_id);
  /**
   * @brief handler for created event of a ivi shell layer
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] layer_id layer id
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_layer_created(void* data,
                                   struct ivi_wm* ivi_wm,
                                   uint32_t layer_id);
  /**
   * @brief handler for destroyed event of a ivi shell surface
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] surface_id surface id
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_surface_destroyed(void* data,
                                       struct ivi_wm* ivi_wm,
                                       uint32_t surface_id);
  /**
   * @brief handler for destroyed event of a ivi shell layer
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] layer_id layer id
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_layer_destroyed(void* data,
                                     struct ivi_wm* ivi_wm,
                                     uint32_t layer_id);
  /**
   * @brief handler for error event of a ivi shell surface
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] object_id wayland object id
   * @param[in] error error code
   * @param[in] message error message
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_surface_error(void* data,
                                   struct ivi_wm* ivi_wm,
                                   uint32_t object_id,
                                   uint32_t error,
                                   const char* message);
  /**
   * @brief handler for error event of a ivi shell layer
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] object_id wayland object id
   * @param[in] error error code
   * @param[in] message error message
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_layer_error(void* data,
                                 struct ivi_wm* ivi_wm,
                                 uint32_t object_id,
                                 uint32_t error,
                                 const char* message);
  /**
   * @brief handler for a size of a ivi shell surface
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] surface_id surface id
   * @param[in] width width of a surface
   * @param[in] height height of a surface
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_surface_size(void* data,
                                  struct ivi_wm* ivi_wm,
                                  uint32_t surface_id,
                                  int32_t width,
                                  int32_t height);
  /**
   * @brief handler for stats of a ivi shell surface
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] surface_id surface id
   * @param[in] frame_count frame count
   * @param[in] pid process id
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_surface_stats(void* data,
                                   struct ivi_wm* ivi_wm,
                                   uint32_t surface_id,
                                   uint32_t frame_count,
                                   uint32_t pid);
  /**
   * @brief handler for surface added event of a ivi shell layer
   * @param[in,out] data Data of type Display
   * @param[in] ivi_wm ivi shell window manager
   * @param[in] layer_id layer id
   * @param[in] surface_id surface id
   * @return void
   * @relation
   * wayland
   */
  static void ivi_wm_layer_surface_added(void* data,
                                         struct ivi_wm* ivi_wm,
                                         uint32_t layer_id,
                                         uint32_t surface_id);

  static const struct ivi_wm_listener ivi_wm_listener;
};
