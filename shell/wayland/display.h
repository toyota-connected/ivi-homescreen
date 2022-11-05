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

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <cassert>
#include "third_party/flutter/flutter_embedder.h"

#include "agl-shell-client-protocol.h"
#include "constants.h"
#include "static_plugins/text_input/text_input.h"
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

  struct wl_compositor* GetCompositor() {
    assert(m_compositor);
    return m_compositor;
  };

  struct wl_subcompositor* GetSubCompositor() {
    assert(m_subcompositor);
    return m_subcompositor;
  };

  struct wl_display* GetDisplay() {
    assert(m_display);
    return m_display;
  }

  struct xdg_wm_base* GetXdgWmBase() { return m_xdg_wm_base; }

  struct wl_shm* GetShm() {
    assert(m_shm);
    return m_shm;
  }

  int PollEvents();

  void AglShellDoBackground(struct wl_surface*, size_t index);

  void AglShellDoPanel(struct wl_surface*,
                       enum agl_shell_edge mode,
                       size_t index);

  void AglShellDoReady() const;

  void SetEngine(wl_surface* surface, Engine* engine);

  bool ActivateSystemCursor(int32_t device, const std::string& kind);

  void SetTextInput(wl_surface* surface, TextInput* text_input);

  wl_output* GetWlOutput(uint32_t index) {
    if (index <= m_all_outputs.size()) {
      return m_all_outputs[index]->output;
    }
    return nullptr;
  }

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
    struct touch_point points[kMaxTouchPoints];
  };

  struct touch {
    struct wl_touch* touch;
    struct touch_event event;
    int down_count[kMaxTouchPoints];

    wl_fixed_t surface_x, surface_y;
    uint32_t state;
    FlutterPointerPhase phase;
  } m_touch{};

  // for cursor
  struct wl_cursor_theme* m_cursor_theme{};

  struct xkb_context* m_xkb_context;
  struct xkb_keymap* m_keymap{};
  struct xkb_state* m_xkb_state{};

  std::map<wl_surface*, TextInput*> m_text_input;

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

  std::vector<std::shared_ptr<output_info_t>> m_all_outputs;
  int32_t m_buffer_scale;
  MAYBE_UNUSED int32_t m_last_buffer_scale;
  bool m_buffer_scale_enable{};

  static const struct wl_registry_listener registry_listener;

  static void registry_handle_global(void* data,
                                     struct wl_registry* registry,
                                     uint32_t name,
                                     const char* interface,
                                     uint32_t version);

  static void registry_handle_global_remove(void* data,
                                            struct wl_registry* reg,
                                            uint32_t id);

  static const struct wl_output_listener output_listener;

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

  static void display_handle_mode(void* data,
                                  struct wl_output* wl_output,
                                  uint32_t flags,
                                  int width,
                                  int height,
                                  int refresh);

  static void display_handle_scale(void* data,
                                   struct wl_output* wl_output,
                                   int scale);

  static void display_handle_done(void* data, struct wl_output* wl_output);

  static const struct wl_surface_listener base_surface_listener;

  static void handle_base_surface_enter(void* data,
                                        struct wl_surface* surface,
                                        struct wl_output* output);

  static void handle_base_surface_leave(void* data,
                                        struct wl_surface* surface,
                                        struct wl_output* output);

  static const struct wl_shm_listener shm_listener;

  static void shm_format(void* data, struct wl_shm* wl_shm, uint32_t format);

  static const struct wl_seat_listener seat_listener;

  static void seat_handle_capabilities(void* data,
                                       struct wl_seat* seat,
                                       uint32_t caps);

  static void seat_handle_name(void* data,
                               struct wl_seat* seat,
                               const char* name);

  static bool pointerButtonStatePressed(struct pointer* p);

  static void pointer_handle_enter(void* data,
                                   struct wl_pointer* pointer,
                                   uint32_t serial,
                                   struct wl_surface* surface,
                                   wl_fixed_t sx,
                                   wl_fixed_t sy);

  static void pointer_handle_leave(void* data,
                                   struct wl_pointer* pointer,
                                   uint32_t serial,
                                   struct wl_surface* surface);

  static void pointer_handle_motion(void* data,
                                    struct wl_pointer* pointer,
                                    uint32_t time,
                                    wl_fixed_t sx,
                                    wl_fixed_t sy);

  static void pointer_handle_button(void* data,
                                    struct wl_pointer* wl_pointer,
                                    uint32_t serial,
                                    uint32_t time,
                                    uint32_t button,
                                    uint32_t state);

  static void pointer_handle_axis(void* data,
                                  struct wl_pointer* wl_pointer,
                                  uint32_t time,
                                  uint32_t axis,
                                  wl_fixed_t value);

  static void pointer_handle_frame(void* data, struct wl_pointer* wl_pointer);

  static void pointer_handle_axis_source(void* data,
                                         struct wl_pointer* wl_pointer,
                                         uint32_t axis_source);

  static void pointer_handle_axis_stop(void* data,
                                       struct wl_pointer* wl_pointer,
                                       uint32_t time,
                                       uint32_t axis);

  static void pointer_handle_axis_discrete(void* data,
                                           struct wl_pointer* wl_pointer,
                                           uint32_t axis,
                                           int32_t discrete);

  static const struct wl_pointer_listener pointer_listener;

  static void keyboard_handle_keymap(void* data,
                                     struct wl_keyboard* keyboard,
                                     uint32_t format,
                                     int fd,
                                     uint32_t size);

  static void keyboard_handle_enter(void* data,
                                    struct wl_keyboard* keyboard,
                                    uint32_t serial,
                                    struct wl_surface* surface,
                                    struct wl_array* keys);

  static void keyboard_handle_leave(void* data,
                                    struct wl_keyboard* keyboard,
                                    uint32_t serial,
                                    struct wl_surface* surface);

  static void keyboard_handle_key(void* data,
                                  struct wl_keyboard* keyboard,
                                  uint32_t serial,
                                  uint32_t time,
                                  uint32_t key,
                                  uint32_t state);

  static void keyboard_handle_modifiers(void* data,
                                        struct wl_keyboard* keyboard,
                                        uint32_t serial,
                                        uint32_t mods_depressed,
                                        uint32_t mods_latched,
                                        uint32_t mods_locked,
                                        uint32_t group);

  static void keyboard_handle_repeat_info(void* data,
                                          struct wl_keyboard* wl_keyboard,
                                          int32_t rate,
                                          int32_t delay);

  static const struct wl_keyboard_listener keyboard_listener;

  static void touch_handle_down(void* data,
                                struct wl_touch* wl_touch,
                                uint32_t serial,
                                uint32_t time,
                                struct wl_surface* surface,
                                int32_t id,
                                wl_fixed_t x_w,
                                wl_fixed_t y_w);

  static void touch_handle_up(void* data,
                              struct wl_touch* wl_touch,
                              uint32_t serial,
                              uint32_t time,
                              int32_t id);

  static void touch_handle_motion(void* data,
                                  struct wl_touch* wl_touch,
                                  uint32_t time,
                                  int32_t id,
                                  wl_fixed_t x_w,
                                  wl_fixed_t y_w);

  static void touch_handle_cancel(void* data, struct wl_touch* wl_touch);

  static void touch_handle_frame(void* data, struct wl_touch* wl_touch);

  static const struct wl_touch_listener touch_listener;

  static void agl_shell_bound_ok(void* data, struct agl_shell* shell);

  static void agl_shell_bound_fail(void* data, struct agl_shell* shell);

  static const struct agl_shell_listener agl_shell_listener;
};
