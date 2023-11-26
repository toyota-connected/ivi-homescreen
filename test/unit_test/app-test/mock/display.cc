// Copyright 2020 Toyota Connected North America
// @copyright Copyright (c) 2022 Woven Alpha, Inc.
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

#include "wayland/display.h"

#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <algorithm>
#include <cstring>
#include <utility>

#include "constants.h"
#include "engine.h"
#include "timer.h"

Display::Display(bool enable_cursor,
                 const std::string& /* ignore_wayland_event */,
                 std::string cursor_theme_name,
                 const std::vector<Configuration::Config>& /* configs */)
    : m_xkb_context(xkb_context_new(XKB_CONTEXT_NO_FLAGS)),
      m_enable_cursor(enable_cursor),
      m_cursor_theme_name(std::move(cursor_theme_name)) {
  /* Delete implementation */
}

Display::~Display() {
  /* Delete implementation */
}

/**
 * @brief Respond to a ping event with a pong request
 * @param[in] data No use
 * @param[in] xdg_wm_base Pointer to xdg_shell interface
 * @param[in] serial Serial of pointer
 * @return void
 * @relation
 * wayland
 */
static void xdg_wm_base_ping(void* /* data */,
                             struct xdg_wm_base* xdg_wm_base,
                             uint32_t serial) {
  xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

void Display::registry_handle_global(void* data,
                                     struct wl_registry* registry,
                                     uint32_t name,
                                     const char* interface,
                                     uint32_t version) {
  auto* d = static_cast<Display*>(data);

  SPDLOG_DEBUG("Wayland: {} version {}", interface, version);

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    if (version >= 3) {
      d->m_compositor = static_cast<struct wl_compositor*>(
          wl_registry_bind(registry, name, &wl_compositor_interface,
                           std::min(static_cast<uint32_t>(3), version)));
      SPDLOG_DEBUG("\tBuffer Scale Enabled");
      d->m_buffer_scale_enable = true;
      if (d->m_shm) {
        d->m_cursor_surface = wl_compositor_create_surface(d->m_compositor);
      }
    } else {
      d->m_compositor = static_cast<struct wl_compositor*>(
          wl_registry_bind(registry, name, &wl_compositor_interface,
                           std::min(static_cast<uint32_t>(2), version)));
    }
    d->m_base_surface = wl_compositor_create_surface(d->m_compositor);
  } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
    d->m_subcompositor = static_cast<struct wl_subcompositor*>(
        wl_registry_bind(registry, name, &wl_subcompositor_interface,
                         std::min(static_cast<uint32_t>(1), version)));
  }
#if defined(ENABLE_XDG_CLIENT)
  else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    d->m_xdg_wm_base = static_cast<struct xdg_wm_base*>(
        wl_registry_bind(registry, name, &xdg_wm_base_interface,
                         std::min(static_cast<uint32_t>(3), version)));
    xdg_wm_base_add_listener(d->m_xdg_wm_base, &xdg_wm_base_listener, d);
  }
#endif
  else if (strcmp(interface, wl_shm_interface.name) == 0) {
    d->m_shm = static_cast<struct wl_shm*>(
        wl_registry_bind(registry, name, &wl_shm_interface,
                         std::min(static_cast<uint32_t>(1), version)));
    wl_shm_add_listener(d->m_shm, &shm_listener, d);

    if (d->m_enable_cursor) {
      d->m_cursor_theme = wl_cursor_theme_load(d->m_cursor_theme_name.c_str(),
                                               kCursorSize, d->m_shm);
    }
    if (d->m_compositor) {
      d->m_cursor_surface = wl_compositor_create_surface(d->m_compositor);
    }
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    auto oi = std::make_shared<output_info_t>();
    std::fill_n(oi.get(), 1, output_info_t{});
    oi->global_id = name;
    oi->output = static_cast<struct wl_output*>(
        wl_registry_bind(registry, name, &wl_output_interface,
                         std::min(static_cast<uint32_t>(2), version)));
    wl_output_add_listener(oi->output, &output_listener, oi.get());
    SPDLOG_DEBUG("Wayland: Output [{}]", d->m_all_outputs.size());
    d->m_all_outputs.push_back(oi);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    d->m_seat = static_cast<wl_seat*>(
        wl_registry_bind(registry, name, &wl_seat_interface,
                         std::min(static_cast<uint32_t>(5), version)));
    wl_seat_add_listener(d->m_seat, &seat_listener, d);

    d->m_repeat_timer =
        std::make_shared<EventTimer>(CLOCK_MONOTONIC, keyboard_repeat_func, d);
    d->m_repeat_timer->set_timerspec(40, 400);
  }
#if defined(ENABLE_AGL_CLIENT)
  else if (strcmp(interface, agl_shell_interface.name) == 0 &&
           d->m_agl.bind_to_agl_shell) {
    if (version >= 2) {
      d->m_agl.shell = static_cast<struct agl_shell*>(
          wl_registry_bind(registry, name, &agl_shell_interface,
                           std::min(static_cast<uint32_t>(2), version)));
      agl_shell_add_listener(d->m_agl.shell, &agl_shell_listener, data);
    } else {
      d->m_agl.shell = static_cast<struct agl_shell*>(
          wl_registry_bind(registry, name, &agl_shell_interface,
                           std::min(static_cast<uint32_t>(1), version)));
    }
    d->m_agl.version = version;
    spdlog::info("Wayland: agl_shell version: {}", version);
  }
#endif
#if defined(ENABLE_IVI_SHELL_CLIENT)
  else if (strcmp(interface, ivi_application_interface.name) == 0) {
    d->m_ivi_shell.application = static_cast<struct ivi_application*>(
        wl_registry_bind(registry, name, &ivi_application_interface, 1));
    spdlog::info("Wayland: ivi_application version: {}", version);
  } else if (strcmp(interface, ivi_wm_interface.name) == 0) {
    d->m_ivi_shell.ivi_wm = static_cast<struct ivi_wm*>(
        wl_registry_bind(registry, name, &ivi_wm_interface, 1));
    ivi_wm_add_listener(d->m_ivi_shell.ivi_wm, &ivi_wm_listener, data);
    spdlog::info("Wayland: ivi_wm version: {}", version);
  }
#endif
}

void Display::registry_handle_global_remove(void* /* data */,
                                            struct wl_registry* /* reg */,
                                            uint32_t /* id */) {}

const struct wl_registry_listener Display::registry_listener = {
    registry_handle_global,
    registry_handle_global_remove,
};

void Display::display_handle_geometry(void* data,
                                      struct wl_output* /* wl_output */,
                                      int /* x */,
                                      int /* y */,
                                      int physical_width,
                                      int physical_height,
                                      int /* subpixel */,
                                      const char* /* make */,
                                      const char* /* model */,
                                      int /* transform */) {
  auto* oi = static_cast<output_info_t*>(data);
  oi->physical_width = static_cast<unsigned int>(physical_width);
  oi->physical_height = static_cast<unsigned int>(physical_height);

  SPDLOG_DEBUG("Physical width: {} mm x {} mm", physical_width,
               physical_height);
}

void Display::display_handle_mode(void* data,
                                  struct wl_output* /* wl_output */,
                                  uint32_t /* flags */,
                                  int width,
                                  int height,
                                  int refresh) {
  auto* oi = static_cast<output_info_t*>(data);
  oi->width = static_cast<unsigned int>(width);
  oi->height = static_cast<unsigned int>(height);
  oi->refresh_rate = refresh;

  SPDLOG_DEBUG("Video mode: {} x {} @ {} Hz", width, height,
               (refresh > 1000 ? refresh / 1000.0 : static_cast<double>(refresh)));
}

void Display::display_handle_scale(void* data,
                                   struct wl_output* /* wl_output */,
                                   int32_t factor) {
  auto* oi = static_cast<output_info_t*>(data);
  oi->scale = factor;

  SPDLOG_DEBUG("Display Scale Factor: {}", factor);
}

void Display::display_handle_done(void* data,
                                  struct wl_output* /* wl_output */) {
  auto* oi = static_cast<output_info_t*>(data);
  oi->done = true;
}

const struct wl_output_listener Display::output_listener = {
    display_handle_geometry, display_handle_mode, display_handle_done,
    display_handle_scale};

void Display::shm_format(void* /* data */,
                         struct wl_shm* /* wl_shm */,
                         uint32_t /* format */) {}

const struct wl_shm_listener Display::shm_listener = {shm_format};

void Display::seat_handle_capabilities(void* data,
                                       struct wl_seat* seat,
                                       uint32_t caps) {
  auto* d = static_cast<Display*>(data);

  if (!d->m_wayland_event_mask.pointer) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->m_pointer.wl_pointer) {
      spdlog::info("Pointer Present");
      d->m_pointer.wl_pointer = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(d->m_pointer.wl_pointer, &pointer_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->m_pointer.wl_pointer) {
      wl_pointer_release(d->m_pointer.wl_pointer);
      d->m_pointer.wl_pointer = nullptr;
    }
  }

  if (!d->m_wayland_event_mask.keyboard) {
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->m_keyboard) {
      spdlog::info("Keyboard Present");
      d->m_keyboard = wl_seat_get_keyboard(seat);
      wl_keyboard_add_listener(d->m_keyboard, &keyboard_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->m_keyboard) {
      wl_keyboard_release(d->m_keyboard);
      d->m_keyboard = nullptr;
    }
  }

  if (!d->m_wayland_event_mask.touch) {
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->m_touch.touch) {
      spdlog::info("Touch Present");
      d->m_touch.touch = wl_seat_get_touch(seat);
      wl_touch_set_user_data(d->m_touch.touch, d);
      wl_touch_add_listener(d->m_touch.touch, &touch_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && d->m_touch.touch) {
      wl_touch_release(d->m_touch.touch);
      d->m_touch.touch = nullptr;
    }
  }
}

void Display::seat_handle_name(void* /* data */,
                               struct wl_seat* /* seat */,
                               const char* name) {
  (void)name;
  SPDLOG_DEBUG("Seat: {}", name);
}

const struct wl_seat_listener Display::seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

bool Display::pointerButtonStatePressed(const struct pointer* p) {
  return (p->buttons) && (p->event.state == WL_POINTER_BUTTON_STATE_PRESSED);
}

void Display::pointer_handle_enter(void* data,
                                   struct wl_pointer* /* pointer */,
                                   const uint32_t serial,
                                   struct wl_surface* surface,
                                   const wl_fixed_t sx,
                                   const wl_fixed_t sy) {
  auto* d = static_cast<Display*>(data);
  d->m_active_surface = surface;
  d->m_active_engine = d->m_surface_engine_map[surface];

  d->m_pointer.event.surface_x = wl_fixed_to_double(sx);
  d->m_pointer.event.surface_y = wl_fixed_to_double(sy);
  d->m_pointer.serial = serial;

  if (d->m_active_engine) {
    d->m_active_engine->CoalesceMouseEvent(
        kFlutterPointerSignalKindNone, FlutterPointerPhase::kAdd,
        d->m_pointer.event.surface_x, d->m_pointer.event.surface_y, 0.0, 0.0,
        d->m_pointer.buttons);
  }
}

void Display::pointer_handle_leave(void* data,
                                   struct wl_pointer* /* pointer */,
                                   uint32_t serial,
                                   struct wl_surface* /* surface */) {
  auto* d = static_cast<Display*>(data);

  d->m_pointer.serial = serial;

  if (d->m_active_engine) {
    d->m_active_engine->CoalesceMouseEvent(kFlutterPointerSignalKindNone,
                                           FlutterPointerPhase::kRemove, 0.0,
                                           0.0, 0.0, 0.0, d->m_pointer.buttons);
  }
}

void Display::pointer_handle_motion(void* data,
                                    struct wl_pointer* /* pointer */,
                                    uint32_t /* time */,
                                    wl_fixed_t sx,
                                    wl_fixed_t sy) {
  auto* d = static_cast<Display*>(data);

  if (!d->m_wayland_event_mask.pointer_motion) {
    d->m_pointer.event.surface_x = wl_fixed_to_double(sx);
    d->m_pointer.event.surface_y = wl_fixed_to_double(sy);

    if (d->m_active_engine) {
      FlutterPointerPhase phase =
          pointerButtonStatePressed(&d->m_pointer) ? kMove : kHover;
      d->m_active_engine->CoalesceMouseEvent(
          kFlutterPointerSignalKindNone, phase, d->m_pointer.event.surface_x,
          d->m_pointer.event.surface_y, 0.0, 0.0, d->m_pointer.buttons);
    }
  }
}

void Display::pointer_handle_button(void* data,
                                    struct wl_pointer* /* wl_pointer */,
                                    uint32_t serial,
                                    uint32_t /* time */,
                                    uint32_t button,
                                    uint32_t state) {
  auto* d = static_cast<Display*>(data);
  if (!d->m_wayland_event_mask.pointer_buttons) {
    d->m_pointer.event.button = button;
    d->m_pointer.event.state = state;
    d->m_pointer.serial = serial;

    if (button == BTN_LEFT)
      d->m_pointer.buttons = kFlutterPointerButtonMousePrimary;
    else if (button == BTN_MIDDLE)
      d->m_pointer.buttons = kFlutterPointerButtonMouseMiddle;
    else if (button == BTN_RIGHT)
      d->m_pointer.buttons = kFlutterPointerButtonMouseSecondary;

    FlutterPointerPhase phase{};
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
      phase = kDown;
    } else if (state == WL_POINTER_BUTTON_STATE_RELEASED) {
      phase = kUp;
    }

    if (d->m_active_engine) {
      d->m_active_engine->CoalesceMouseEvent(
          kFlutterPointerSignalKindNone, phase, d->m_pointer.event.surface_x,
          d->m_pointer.event.surface_y, 0.0, 0.0, d->m_pointer.buttons);
    }
  }
}

void Display::pointer_handle_axis(void* data,
                                  struct wl_pointer* /* wl_pointer */,
                                  uint32_t time,
                                  uint32_t axis,
                                  wl_fixed_t value) {
  auto* d = static_cast<Display*>(data);
  if (!d->m_wayland_event_mask.pointer_axis) {
    d->m_pointer.event.time = time;
    d->m_pointer.event.axes[axis].value = wl_fixed_to_double(value);

    if (d->m_active_engine) {
      d->m_active_engine->CoalesceMouseEvent(
          kFlutterPointerSignalKindScroll, FlutterPointerPhase::kMove,
          d->m_pointer.event.surface_x, d->m_pointer.event.surface_y,
          d->m_pointer.event.axes[1].value, d->m_pointer.event.axes[0].value,
          d->m_pointer.buttons);
    }
  }
}

void Display::pointer_handle_frame(void* /* data */,
                                   struct wl_pointer* /* wl_pointer */) {}

void Display::pointer_handle_axis_source(void* /* data */,
                                         struct wl_pointer* /* wl_pointer */,
                                         uint32_t /* axis_source */) {}

void Display::pointer_handle_axis_stop(void* /* data */,
                                       struct wl_pointer* /* wl_pointer */,
                                       uint32_t /* time */,
                                       uint32_t /* axis */) {}

void Display::pointer_handle_axis_discrete(void* /* data */,
                                           struct wl_pointer* /* wl_pointer */,
                                           uint32_t /* axis */,
                                           int32_t /* discrete */) {}

const struct wl_pointer_listener Display::pointer_listener = {
    .enter = pointer_handle_enter,
    .leave = pointer_handle_leave,
    .motion = pointer_handle_motion,
    .button = pointer_handle_button,
    .axis = pointer_handle_axis,
    .frame = pointer_handle_frame,
    .axis_source = pointer_handle_axis_source,
    .axis_stop = pointer_handle_axis_stop,
    .axis_discrete = pointer_handle_axis_discrete,
};

void Display::keyboard_handle_enter(void* data,
                                    struct wl_keyboard* /* keyboard */,
                                    uint32_t /* serial */,
                                    struct wl_surface* surface,
                                    struct wl_array* /* keys */) {
  auto* d = static_cast<Display*>(data);
  d->m_active_surface = surface;
  d->m_active_engine = d->m_surface_engine_map[surface];
}

void Display::keyboard_handle_leave(void* data,
                                    struct wl_keyboard* /* keyboard */,
                                    uint32_t /* serial */,
                                    struct wl_surface* /* surface */) {
  auto* d = static_cast<Display*>(data);

  d->m_repeat_timer->disarm();
}

void Display::keyboard_handle_keymap(void* data,
                                     struct wl_keyboard* /* keyboard */,
                                     uint32_t /* format */,
                                     int fd,
                                     uint32_t size) {
  auto* d = static_cast<Display*>(data);
  char* keymap_string =
      static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
  xkb_keymap_unref(d->m_keymap);
  d->m_keymap = xkb_keymap_new_from_string(d->m_xkb_context, keymap_string,
                                           XKB_KEYMAP_FORMAT_TEXT_V1,
                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(keymap_string, size);
  close(fd);
  xkb_state_unref(d->m_xkb_state);
  d->m_xkb_state = xkb_state_new(d->m_keymap);
}

void Display::keyboard_handle_key(void* data,
                                  struct wl_keyboard* /* keyboard */,
                                  uint32_t /* serial */,
                                  uint32_t /* time */,
                                  uint32_t key,
                                  uint32_t state) {
  auto* d = static_cast<Display*>(data);

  if (!d->m_xkb_state)
    return;

  //
  // Important: the scancode from this event is the Linux evdev scancode.
  // To translate this to an XKB scancode, you must add 8 to the evdev scancode.
  //
  uint32_t xkb_scancode = key + 8;
  //
  // Gets the single keysym obtained from pressing a particular key in a given
  // keyboard state. If the key does not have exactly one keysym, returns
  // XKB_KEY_NoSymbol
  //
  xkb_keysym_t keysym = xkb_state_key_get_one_sym(d->m_xkb_state, xkb_scancode);
  if (keysym == XKB_KEY_NoSymbol) {
    const xkb_keysym_t* key_symbols;
    int res =
        xkb_state_key_get_syms(d->m_xkb_state, xkb_scancode, &key_symbols);
    if (res == 0) {
      spdlog::info("xkb_scancode has no key symbols: 0x{:x}", xkb_scancode);
      keysym = XKB_KEY_NoSymbol;
    } else {
      // only use the first symbol until the use case for two is clarified
      keysym = key_symbols[0];
      for (int i = 0; i < res; i++) {
        spdlog::info("xkb keysym: 0x{:x}", key_symbols[i]);
      }
    }
  }

#if ENABLE_PLUGIN_TEXT_INPUT
  auto text_input = d->m_text_input[d->m_active_surface];
#endif
#if ENABLE_PLUGIN_KEY_EVENT
  auto key_event = d->m_key_event[d->m_active_surface];
  std::shared_ptr<DelegateHandleKey> delegate = nullptr;
#endif

  if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    if (xkb_keymap_key_repeats(d->m_keymap, xkb_scancode)) {
      d->m_keysym_pressed = keysym;
      d->m_repeat_code = xkb_scancode;
      d->m_repeat_timer->arm();
    } else {
      SPDLOG_DEBUG("key does not repeat: 0x{:x}", xkb_scancode);
    }

#if ENABLE_PLUGIN_TEXT_INPUT
    if (text_input) {
#endif
#if ENABLE_PLUGIN_KEY_EVENT && ENABLE_PLUGIN_TEXT_INPUT
      // The both of TextInput and KeyEvent is enabled.
      delegate = std::move(
          TextInput::GetDelegate(text_input, kFlutterKeyEventTypeRepeat,
                                 d->m_repeat_code, d->m_keysym_pressed));
#elif !(ENABLE_PLUGIN_KEY_EVENT) && ENABLE_PLUGIN_TEXT_INPUT
    // Only TextInput is enabled.
    TextInput::keyboard_handle_key(text_input, keysym, state);
#endif
#if ENABLE_PLUGIN_TEXT_INPUT
    }
#endif

#if ENABLE_PLUGIN_KEY_EVENT
    if (key_event) {
      KeyEvent::keyboard_handle_key(key_event, kFlutterKeyEventTypeDown,
                                    xkb_scancode, keysym, std::move(delegate));
    }
#endif
  } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
#if ENABLE_PLUGIN_KEY_EVENT
    if (key_event) {
      KeyEvent::keyboard_handle_key(key_event, kFlutterKeyEventTypeUp,
                                    xkb_scancode, keysym, nullptr);
    }
#endif
    if (d->m_repeat_code == xkb_scancode) {
      d->m_repeat_timer->disarm();
      d->m_repeat_code = XKB_KEY_NoSymbol;
    }
  }
}

void Display::keyboard_handle_modifiers(void* data,
                                        struct wl_keyboard* /* keyboard */,
                                        uint32_t /* serial */,
                                        uint32_t mods_depressed,
                                        uint32_t mods_latched,
                                        uint32_t mods_locked,
                                        uint32_t group) {
  auto* d = static_cast<Display*>(data);
  xkb_state_update_mask(d->m_xkb_state, mods_depressed, mods_latched,
                        mods_locked, 0, 0, group);
}

void Display::keyboard_handle_repeat_info(void* data,
                                          struct wl_keyboard* /* wl_keyboard */,
                                          int32_t rate,
                                          int32_t delay) {
  auto d = reinterpret_cast<Display*>(data);

  d->m_repeat_timer->set_timerspec(rate, delay);

  SPDLOG_DEBUG("[keyboard repeat info] rate: {}, delay: {}", rate, delay);
}

const struct wl_keyboard_listener Display::keyboard_listener = {
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .key = keyboard_handle_key,
    .modifiers = keyboard_handle_modifiers,
    .repeat_info = keyboard_handle_repeat_info,
};

void Display::keyboard_repeat_func(void* data) {
  auto d = reinterpret_cast<Display*>(data);

#if ENABLE_PLUGIN_TEXT_INPUT
  auto text_input = d->m_text_input[d->m_active_surface];
#endif
#if ENABLE_PLUGIN_KEY_EVENT
  auto key_event = d->m_key_event[d->m_active_surface];
  std::shared_ptr<DelegateHandleKey> delegate = nullptr;
#endif

#if ENABLE_PLUGIN_TEXT_INPUT
  if (text_input && d->m_repeat_code != XKB_KEY_NoSymbol) {
#endif
#if ENABLE_PLUGIN_KEY_EVENT && ENABLE_PLUGIN_TEXT_INPUT
    // The both of TextInput and KeyEvent is enabled.
    delegate = std::move(
        TextInput::GetDelegate(text_input, kFlutterKeyEventTypeRepeat,
                               d->m_repeat_code, d->m_keysym_pressed));
#elif !(ENABLE_PLUGIN_KEY_EVENT) && ENABLE_PLUGIN_TEXT_INPUT
  // Only TextInput is enabled.
  TextInput::keyboard_handle_key(text_input, d->m_keysym_pressed,
                                 WL_KEYBOARD_KEY_STATE_PRESSED);
#endif
#if ENABLE_PLUGIN_TEXT_INPUT
  }
#endif

#if ENABLE_PLUGIN_KEY_EVENT
  if (key_event && d->m_repeat_code != XKB_KEY_NoSymbol) {
    KeyEvent::keyboard_handle_key(key_event, kFlutterKeyEventTypeRepeat,
                                  d->m_repeat_code, d->m_keysym_pressed,
                                  std::move(delegate));
  }
#endif
}

void Display::touch_handle_down(void* data,
                                struct wl_touch* /* wl_touch */,
                                uint32_t /* serial */,
                                uint32_t /* time */,
                                struct wl_surface* surface,
                                int32_t id,
                                wl_fixed_t x_w,
                                wl_fixed_t y_w) {
  auto* d = static_cast<Display*>(data);

  d->m_touch.surface_x[id] = x_w;
  d->m_touch.surface_y[id] = y_w;

  d->m_active_surface = surface;
  d->m_touch_engine = d->m_surface_engine_map[surface];
  if (d->m_touch_engine) {
    d->m_touch_engine->CoalesceTouchEvent(FlutterPointerPhase::kDown,
                                          wl_fixed_to_double(x_w),
                                          wl_fixed_to_double(y_w), id);
  }
}

void Display::touch_handle_up(void* data,
                              struct wl_touch* /* wl_touch */,
                              uint32_t /* serial */,
                              uint32_t /* time */,
                              int32_t id) {
  auto* d = static_cast<Display*>(data);

  if (d->m_touch_engine) {
    d->m_touch_engine->CoalesceTouchEvent(
        FlutterPointerPhase::kUp, wl_fixed_to_double(d->m_touch.surface_x[id]),
        wl_fixed_to_double(d->m_touch.surface_y[id]), id);
  }
}

void Display::touch_handle_motion(void* data,
                                  struct wl_touch* /* wl_touch */,
                                  uint32_t /* time */,
                                  int32_t id,
                                  wl_fixed_t x_w,
                                  wl_fixed_t y_w) {
  auto* d = static_cast<Display*>(data);

  d->m_touch.surface_x[id] = x_w;
  d->m_touch.surface_y[id] = y_w;

  if (d->m_touch_engine) {
    d->m_touch_engine->CoalesceTouchEvent(FlutterPointerPhase::kMove,
                                          wl_fixed_to_double(x_w),
                                          wl_fixed_to_double(y_w), id);
  }
}

void Display::touch_handle_cancel(void* data, struct wl_touch* /* wl_touch */) {
  auto* d = static_cast<Display*>(data);
  if (d->m_touch_engine) {
    SPDLOG_DEBUG("touch_handle_cancel");
    d->m_touch_engine->CoalesceTouchEvent(FlutterPointerPhase::kCancel,
                                          d->m_pointer.event.surface_x,
                                          d->m_pointer.event.surface_y, 0);
  }
}

void Display::touch_handle_frame(void* /* data */,
                                 struct wl_touch* /* wl_touch */) {}

const struct wl_touch_listener Display::touch_listener = {
    .down = touch_handle_down,
    .up = touch_handle_up,
    .motion = touch_handle_motion,
    .frame = touch_handle_frame,
    .cancel = touch_handle_cancel,
};

int Display::PollEvents() const{
  /* Delete implementation */
  return 1;
}

void Display::AglShellDoBackground(struct wl_surface* surface, size_t index) const {
  if (m_agl.shell) {
    agl_shell_set_background(m_agl.shell, surface,
                             m_all_outputs[index]->output);
  }
}

void Display::AglShellDoPanel(struct wl_surface* surface,
                              enum agl_shell_edge mode,
                              size_t index) const {
  if (m_agl.shell) {
    agl_shell_set_panel(m_agl.shell, surface, m_all_outputs[index]->output,
                        mode);
  }
}

void Display::AglShellDoReady() const {
  if (m_agl.shell) {
    agl_shell_ready(m_agl.shell);
  }
}

void Display::AglShellDoSetupActivationArea(const uint32_t x,
                                            const uint32_t y,
                                            const uint32_t index) const {
  uint32_t width = m_all_outputs[index]->width;
  uint32_t height = m_all_outputs[index]->height - (2 * y);

  if (!m_agl.shell)
    return;

  if (m_all_outputs[index]->transform == WL_OUTPUT_TRANSFORM_90) {
    width = m_all_outputs[index]->height;
    height = m_all_outputs[index]->width - (2 * y);
  }

  SPDLOG_DEBUG("Using custom rectangle [{}x{}+{}x{}] for activation", width,
               height, x, y);

  agl_shell_set_activate_region(m_agl.shell, m_all_outputs[index]->output, x, y,
                                width, height);
}

void Display::SetEngine(wl_surface* surface, Engine* engine) {
  m_active_engine = engine;
  m_active_surface = surface;
  m_surface_engine_map[surface] = engine;
}

bool Display::ActivateSystemCursor(const int32_t device, const std::string& kind) const {
  (void)device;
  if (!m_enable_cursor) {
    wl_pointer_set_cursor(m_pointer.wl_pointer, m_pointer.serial, m_cursor_surface,
                          0, 0);
    wl_surface_damage(m_cursor_surface, 0, 0, 0, 0);
    wl_surface_commit(m_cursor_surface);
    return true;
  }

  if (m_pointer.wl_pointer) {
    const char* cursor_name;
    if (kind == "basic") {
      cursor_name = kCursorKindBasic;
    } else if (kind == "click") {
      cursor_name = kCursorKindClick;
    } else if (kind == "text") {
      cursor_name = kCursorKindText;
    } else if (kind == "forbidden") {
      cursor_name = kCursorKindForbidden;
    } else {
      SPDLOG_DEBUG("Cursor Kind = {}", kind);
      return false;
    }

    auto cursor = wl_cursor_theme_get_cursor(m_cursor_theme, cursor_name);
    if (cursor == nullptr) {
      SPDLOG_DEBUG("Cursor [{}] not found", cursor_name);
      return false;
    }
    auto cursor_buffer = wl_cursor_image_get_buffer(cursor->images[0]);
    if (cursor_buffer && m_cursor_surface) {
      wl_pointer_set_cursor(m_pointer.wl_pointer, m_pointer.serial,
                            m_cursor_surface,
                            static_cast<int32_t>(cursor->images[0]->hotspot_x),
                            static_cast<int32_t>(cursor->images[0]->hotspot_y));
      wl_surface_attach(m_cursor_surface, cursor_buffer, 0, 0);
      wl_surface_damage(m_cursor_surface, 0, 0,
                        static_cast<int32_t>(cursor->images[0]->width),
                        static_cast<int32_t>(cursor->images[0]->height));
      wl_surface_commit(m_cursor_surface);
    } else {
      SPDLOG_DEBUG("Failed to set cursor: Invalid Cursor Buffer");
      return false;
    }
  }

  return true;
}

void Display::SetTextInput(wl_surface* surface, TextInput* text_input) {
  m_text_input[surface] = text_input;
}

void Display::SetKeyEvent(wl_surface* surface, KeyEvent* key_event) {
  m_key_event[surface] = key_event;
}

int32_t Display::GetBufferScale(uint32_t index) const {
  if (index < m_all_outputs.size()) {
    if (m_buffer_scale_enable) {
      if (m_all_outputs[index]->scale == 0) {
        return 1;
      } else {
        return m_all_outputs[index]->scale;
      }
    } else {
      return static_cast<int32_t>(kDefaultBufferScale);
    }
  }
  SPDLOG_DEBUG("GetBufferScale: Invalid output index: {}", index);
  return static_cast<int32_t>(kDefaultBufferScale);
}

std::pair<int32_t, int32_t> Display::GetVideoModeSize(uint32_t index) const {
  if (index < m_all_outputs.size()) {
    return {m_all_outputs[index]->width, m_all_outputs[index]->height};
  }
  SPDLOG_DEBUG("GetVideoModeSize: Invalid output index: {}", index);
  return {0, 0};
}

void Display::agl_shell_bound_ok(void* data, struct agl_shell* shell) {
  (void)shell;
  auto* d = static_cast<Display*>(data);
  d->m_agl.wait_for_bound = false;
  d->m_agl.bound_ok = true;
}

void Display::agl_shell_bound_fail(void* data, struct agl_shell* shell) {
  (void)shell;
  auto* d = static_cast<Display*>(data);
  d->m_agl.wait_for_bound = false;
  d->m_agl.bound_ok = false;
}

const struct agl_shell_listener Display::agl_shell_listener = {
    .bound_ok = agl_shell_bound_ok,
    .bound_fail = agl_shell_bound_fail,
};

void Display::ivi_wm_surface_visibility(void* /* data */,
                                        struct ivi_wm* /* ivi_wm */,
                                        uint32_t surface_id,
                                        int32_t visibility) {
  (void)surface_id;
  (void)visibility;
  SPDLOG_DEBUG("ivi_wm_surface_visibility: {}, visibility: {}", surface_id,
               visibility);
}

void Display::ivi_wm_layer_visibility(void* /* data */,
                                      struct ivi_wm* /* ivi_wm */,
                                      uint32_t layer_id,
                                      int32_t visibility) {
  (void)layer_id;
  (void)visibility;
  SPDLOG_DEBUG("ivi_wm_layer_visibility: {}, visibility: {}", layer_id,
               visibility);
}

void Display::ivi_wm_surface_opacity(void* /* data */,
                                     struct ivi_wm* /* ivi_wm */,
                                     uint32_t surface_id,
                                     wl_fixed_t opacity) {
  (void)surface_id;
  (void)opacity;
  SPDLOG_DEBUG("ivi_wm_surface_opacity: {}, opacity: {}", surface_id, opacity);
}

void Display::ivi_wm_layer_opacity(void* /* data */,
                                   struct ivi_wm* /* ivi_wm */,
                                   uint32_t layer_id,
                                   wl_fixed_t opacity) {
  (void)layer_id;
  (void)opacity;
  SPDLOG_DEBUG("ivi_wm_layer_opacity: {}, opacity: {}", layer_id, opacity);
}

void Display::ivi_wm_surface_source_rectangle(void* /* data */,
                                              struct ivi_wm* /* ivi_wm */,
                                              uint32_t surface_id,
                                              int32_t x,
                                              int32_t y,
                                              int32_t width,
                                              int32_t height) {
  (void)surface_id;
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  SPDLOG_DEBUG(
      "ivi_wm_surface_source_rectangle: {}, x: {}, y: {}, width: {}, height: "
      "{}",
      surface_id, x, y, width, height);
}

void Display::ivi_wm_layer_source_rectangle(void* /* data */,
                                            struct ivi_wm* /* ivi_wm */,
                                            uint32_t layer_id,
                                            int32_t x,
                                            int32_t y,
                                            int32_t width,
                                            int32_t height) {
  (void)layer_id;
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  SPDLOG_DEBUG(
      "ivi_wm_layer_source_rectangle: {}, x: {}, y: {}, width: {}, height: {}",
      layer_id, x, y, width, height);
}

void Display::ivi_wm_surface_destination_rectangle(void* /* data */,
                                                   struct ivi_wm* /* ivi_wm */,
                                                   uint32_t surface_id,
                                                   int32_t x,
                                                   int32_t y,
                                                   int32_t width,
                                                   int32_t height) {
  (void)surface_id;
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  SPDLOG_DEBUG(
      "ivi_wm_surface_destination_rectangle: {}, x: {}, y: {}, width: {}, "
      "height: {}",
      surface_id, x, y, width, height);
}

void Display::ivi_wm_layer_destination_rectangle(void* /* data */,
                                                 struct ivi_wm* /* ivi_wm */,
                                                 uint32_t layer_id,
                                                 int32_t x,
                                                 int32_t y,
                                                 int32_t width,
                                                 int32_t height) {
  (void)layer_id;
  (void)x;
  (void)y;
  (void)width;
  (void)height;
  SPDLOG_DEBUG(
      "ivi_wm_surface_destination_rectangle: {}, , x: {}, y: {}, width: {}, "
      "height: {}",
      layer_id, x, y, width, height);
}

void Display::ivi_wm_surface_created(void* /* data */,
                                     struct ivi_wm* /* ivi_wm */,
                                     uint32_t surface_id) {
  (void)surface_id;
  SPDLOG_DEBUG("ivi_wm_surface_created: {}", surface_id);
}

void Display::ivi_wm_layer_created(void* /* data */,
                                   struct ivi_wm* /* ivi_wm */,
                                   uint32_t layer_id) {
  (void)layer_id;
  SPDLOG_DEBUG("ivi_wm_layer_created: {}", layer_id);
}

void Display::ivi_wm_surface_destroyed(void* /* data */,
                                       struct ivi_wm* /* ivi_wm */,
                                       uint32_t surface_id) {
  (void)surface_id;
  SPDLOG_DEBUG("ivi_wm_surface_destroyed: {}", surface_id);
}

void Display::ivi_wm_layer_destroyed(void* /* data */,
                                     struct ivi_wm* /* ivi_wm */,
                                     uint32_t layer_id) {
  (void)layer_id;
  SPDLOG_DEBUG("ivi_wm_layer_destroyed: {}", layer_id);
}

void Display::ivi_wm_surface_error(void* /* data */,
                                   struct ivi_wm* /* ivi_wm */,
                                   uint32_t object_id,
                                   uint32_t error,
                                   const char* message) {
  (void)object_id;
  (void)error;
  (void)message;
  SPDLOG_DEBUG("ivi_wm_surface_error: {}, error ({}), {}", object_id, error,
               message);
}

void Display::ivi_wm_layer_error(void* /* data */,
                                 struct ivi_wm* /* ivi_wm */,
                                 uint32_t object_id,
                                 uint32_t error,
                                 const char* message) {
  (void)object_id;
  (void)error;
  (void)message;
  SPDLOG_DEBUG("ivi_wm_layer_error: {}, error ({}), {}", object_id, error,
               message);
}

void Display::ivi_wm_surface_size(void* /* data */,
                                  struct ivi_wm* /* ivi_wm */,
                                  uint32_t surface_id,
                                  int32_t width,
                                  int32_t height) {
  (void)surface_id;
  (void)width;
  (void)height;
  SPDLOG_DEBUG("ivi_wm_surface_size: {}, width {}, height {}", surface_id,
               width, height);
}

void Display::ivi_wm_surface_stats(void* /* data */,
                                   struct ivi_wm* /* ivi_wm */,
                                   uint32_t surface_id,
                                   uint32_t frame_count,
                                   uint32_t pid) {
  (void)surface_id;
  (void)frame_count;
  (void)pid;
  SPDLOG_DEBUG("ivi_wm_surface_stats: {}, frame_count {}, pid {}", surface_id,
               frame_count, pid);
}

void Display::ivi_wm_layer_surface_added(void* /* data */,
                                         struct ivi_wm* /* ivi_wm */,
                                         uint32_t layer_id,
                                         uint32_t surface_id) {
  (void)layer_id;
  (void)surface_id;
  SPDLOG_DEBUG("ivi_wm_layer_surface_added: {}, {}", layer_id, surface_id);
}

const struct ivi_wm_listener Display::ivi_wm_listener = {
    .surface_visibility = ivi_wm_surface_visibility,
    .layer_visibility = ivi_wm_layer_visibility,
    .surface_opacity = ivi_wm_surface_opacity,
    .layer_opacity = ivi_wm_layer_opacity,
    .surface_source_rectangle = ivi_wm_surface_source_rectangle,
    .layer_source_rectangle = ivi_wm_layer_source_rectangle,
    .surface_destination_rectangle = ivi_wm_surface_destination_rectangle,
    .layer_destination_rectangle = ivi_wm_layer_destination_rectangle,
    .surface_created = ivi_wm_surface_created,
    .layer_created = ivi_wm_layer_created,
    .surface_destroyed = ivi_wm_surface_destroyed,
    .layer_destroyed = ivi_wm_layer_destroyed,
    .surface_error = ivi_wm_surface_error,
    .layer_error = ivi_wm_layer_error,
    .surface_size = ivi_wm_surface_size,
    .surface_stats = ivi_wm_surface_stats,
    .layer_surface_added = ivi_wm_layer_surface_added,
};

void Display::wayland_event_mask_print(const struct wayland_event_mask& mask) {
  std::string out;
  std::stringstream ss(out);
  ss << "Wayland Event Mask";
  if (mask.pointer)
    ss << "\n\tpointer";
  if (mask.pointer_axis)
    ss << "\n\tpointer-axis";
  if (mask.pointer_buttons)
    ss << "\n\tpointer-buttons";
  if (mask.pointer_motion)
    ss << "\n\tpointer-motion";
  if (mask.keyboard)
    ss << "\n\tkeyboard";
  if (mask.touch)
    ss << "\n\ttouch";

  spdlog::info(ss.str());
}

void Display::wayland_event_mask_update(
    const std::string& ignore_wayland_events,
    struct wayland_event_mask& mask) {
  std::string events;
  events.reserve(ignore_wayland_events.size());
  for (const char event : ignore_wayland_events) {
    if (event != ' ' && event != '"')
      events += event;
  }

  std::transform(events.begin(), events.end(), events.begin(),
                 [](char c) { return std::tolower(static_cast<unsigned char>(c)); });

  std::stringstream ss(events);
  while (ss.good()) {
    std::string event;
    getline(ss, event, ',');
    if (event == "pointer")
      mask.pointer = true;
    else if (event == "pointer-axis")
      mask.pointer_axis = true;
    else if (event == "pointer-buttons")
      mask.pointer_buttons = true;
    else if (event == "pointer-motion")
      mask.pointer_motion = true;
    else if (event == "keyboard")
      mask.keyboard = true;
    else if (event == "touch")
      mask.touch = true;
    else if (!event.empty())
      spdlog::warn("Unknown Wayland Event Mask: [{}]", event);
  }
  if (!ignore_wayland_events.empty()) {
    wayland_event_mask_print(mask);
  }
}
