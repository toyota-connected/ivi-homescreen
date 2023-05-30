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

#include "display.h"

#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "constants.h"
#include "engine.h"
#include "timer.h"

Display::Display(bool enable_cursor,
                 std::string cursor_theme_name,
                 const std::vector<Configuration::Config>& configs)
    : m_xkb_context(xkb_context_new(XKB_CONTEXT_NO_FLAGS)),
      m_enable_cursor(enable_cursor),
      m_cursor_theme_name(std::move(cursor_theme_name)) {
  FML_DLOG(INFO) << "+ Display()";

  for (auto const& cfg : configs) {
    // check if we actually need to bind to agl-shell
    auto window_type = WaylandWindow::get_window_type(cfg.view.window_type);
    if (window_type != WaylandWindow::WINDOW_NORMAL) {
      m_agl.bind_to_agl_shell = true;
      break;
    }
  }

  m_display = wl_display_connect(nullptr);
  if (m_display == nullptr) {
    FML_LOG(ERROR) << "Failed to connect to Wayland display. "
                   << strerror(errno);
    exit(-1);
  }

  m_registry = wl_display_get_registry(m_display);
  wl_registry_add_listener(m_registry, &registry_listener, this);
  wl_display_dispatch(m_display);

  if (m_agl.shell && m_agl.bind_to_agl_shell && m_agl.version >= 2) {
    int ret = 0;
    while (ret != -1 && m_agl.wait_for_bound) {
      ret = wl_display_dispatch(m_display);
      if (m_agl.wait_for_bound)
        continue;
    }
    if (!m_agl.bound_ok) {
      FML_LOG(ERROR)
          << "agl_shell extension already in use by other shell client.";
      exit(EXIT_FAILURE);
    }
  }

  if (!m_agl.shell && m_agl.bind_to_agl_shell) {
    FML_LOG(INFO) << "agl_shell extension not present";
  }

  FML_DLOG(INFO) << "- Display()";
}

Display::~Display() {
  FML_DLOG(INFO) << "+ ~Display()";

  if (m_shm)
    wl_shm_destroy(m_shm);

  if (m_agl.shell)
    agl_shell_destroy(m_agl.shell);

  if (m_subcompositor)
    wl_subcompositor_destroy(m_subcompositor);

  if (m_compositor)
    wl_compositor_destroy(m_compositor);

  if (m_cursor_theme)
    wl_cursor_theme_destroy(m_cursor_theme);

  if (m_cursor_surface)
    wl_surface_destroy(m_cursor_surface);

  if (m_xdg_wm_base)
    xdg_wm_base_destroy(m_xdg_wm_base);

  wl_registry_destroy(m_registry);
  wl_display_flush(m_display);
  wl_display_disconnect(m_display);

  FML_DLOG(INFO) << "- ~Display()";
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
static void xdg_wm_base_ping(void* data,
                             struct xdg_wm_base* xdg_wm_base,
                             uint32_t serial) {
  (void)data;
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

  FML_DLOG(INFO) << "Wayland: " << interface << " version " << version;

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    if (version >= 3) {
      d->m_compositor = static_cast<struct wl_compositor*>(
          wl_registry_bind(registry, name, &wl_compositor_interface,
                           std::min(static_cast<uint32_t>(3), version)));
      FML_DLOG(INFO) << "\tBuffer Scale Enabled";
      d->m_buffer_scale_enable = true;
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
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    d->m_xdg_wm_base = static_cast<struct xdg_wm_base*>(
        wl_registry_bind(registry, name, &xdg_wm_base_interface,
                         std::min(static_cast<uint32_t>(3), version)));
    xdg_wm_base_add_listener(d->m_xdg_wm_base, &xdg_wm_base_listener, d);
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    d->m_shm = static_cast<struct wl_shm*>(
        wl_registry_bind(registry, name, &wl_shm_interface,
                         std::min(static_cast<uint32_t>(1), version)));
    wl_shm_add_listener(d->m_shm, &shm_listener, d);

    if (d->m_enable_cursor) {
      d->m_cursor_theme = wl_cursor_theme_load(d->m_cursor_theme_name.c_str(),
                                               kCursorSize, d->m_shm);
    }
    d->m_cursor_surface = wl_compositor_create_surface(d->m_compositor);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    auto oi = std::make_shared<output_info_t>();
    std::fill_n(oi.get(), 1, output_info_t{});
    oi->global_id = name;
    oi->output = static_cast<struct wl_output*>(
        wl_registry_bind(registry, name, &wl_output_interface,
                         std::min(static_cast<uint32_t>(2), version)));
    wl_output_add_listener(oi->output, &output_listener, oi.get());
    FML_DLOG(INFO) << "Wayland: Output [" << d->m_all_outputs.size() << "]";
    d->m_all_outputs.push_back(oi);
  } else if (strcmp(interface, wl_seat_interface.name) == 0) {
    d->m_seat = static_cast<wl_seat*>(
        wl_registry_bind(registry, name, &wl_seat_interface,
                         std::min(static_cast<uint32_t>(5), version)));
    wl_seat_add_listener(d->m_seat, &seat_listener, d);

    d->m_repeat_timer =
        std::make_shared<EventTimer>(CLOCK_MONOTONIC, keyboard_repeat_func, d);
    d->m_repeat_timer->set_timerspec(40, 400);

  } else if (strcmp(interface, agl_shell_interface.name) == 0 &&
             d->m_agl.bind_to_agl_shell) {
    if (version >= 2) {
      d->m_agl.shell = static_cast<struct agl_shell*>(
          wl_registry_bind(registry, name, &agl_shell_interface,
                           std::min(static_cast<uint32_t>(4), version)));
      agl_shell_add_listener(d->m_agl.shell, &agl_shell_listener, data);
    } else {
      d->m_agl.shell = static_cast<struct agl_shell*>(
          wl_registry_bind(registry, name, &agl_shell_interface,
                           std::min(static_cast<uint32_t>(1), version)));
    }
    d->m_agl.version = version;
    FML_LOG(INFO) << "agl_shell version: " << version;
  }
}

void Display::registry_handle_global_remove(void* data,
                                            struct wl_registry* reg,
                                            uint32_t id) {
  (void)data;
  (void)reg;
  (void)id;
}

const struct wl_registry_listener Display::registry_listener = {
    registry_handle_global,
    registry_handle_global_remove,
};

void Display::display_handle_geometry(void* data,
                                      struct wl_output* wl_output,
                                      int x,
                                      int y,
                                      int physical_width,
                                      int physical_height,
                                      int subpixel,
                                      const char* make,
                                      const char* model,
                                      int transform) {
  (void)data;
  (void)wl_output;
  (void)x;
  (void)y;
  (void)subpixel;
  (void)make;
  (void)model;
  (void)transform;

  auto* oi = static_cast<output_info_t*>(data);
  oi->physical_width = static_cast<unsigned int>(physical_width);
  oi->physical_height = static_cast<unsigned int>(physical_height);
  oi->transform = transform;

  FML_DLOG(INFO) << "Physical width: " << physical_width << " mm x "
                 << physical_height << " mm";
}

void Display::display_handle_mode(void* data,
                                  struct wl_output* wl_output,
                                  uint32_t flags,
                                  int width,
                                  int height,
                                  int refresh) {
  (void)wl_output;
  (void)flags;
  auto* oi = static_cast<output_info_t*>(data);

  if (flags == WL_OUTPUT_MODE_CURRENT) {
    oi->height = static_cast<unsigned int>(height);
    oi->width = static_cast<unsigned int>(width);
    oi->refresh_rate = refresh;
  }

  FML_DLOG(INFO) << "Video mode: " << width << " x " << height << " @ "
                 << (refresh > 1000 ? refresh / 1000.0 : (double)refresh)
                 << " Hz";
}

void Display::display_handle_scale(void* data,
                                   struct wl_output* wl_output,
                                   int32_t factor) {
  (void)wl_output;
  auto* oi = static_cast<output_info_t*>(data);
  oi->scale = factor;

  FML_DLOG(INFO) << "Display Scale Factor: " << factor;
}

void Display::display_handle_done(void* data, struct wl_output* wl_output) {
  (void)wl_output;
  auto* oi = static_cast<output_info_t*>(data);
  oi->done = true;
}

const struct wl_output_listener Display::output_listener = {
    display_handle_geometry, display_handle_mode, display_handle_done,
    display_handle_scale};

void Display::shm_format(void* data, struct wl_shm* wl_shm, uint32_t format) {
  (void)data;
  (void)wl_shm;
  (void)format;
}

const struct wl_shm_listener Display::shm_listener = {shm_format};

void Display::seat_handle_capabilities(void* data,
                                       struct wl_seat* seat,
                                       uint32_t caps) {
  auto* d = static_cast<Display*>(data);

  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->m_pointer.pointer) {
    FML_LOG(INFO) << "Pointer Present";
    d->m_pointer.pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(d->m_pointer.pointer, &pointer_listener, d);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && d->m_pointer.pointer) {
    wl_pointer_release(d->m_pointer.pointer);
    d->m_pointer.pointer = nullptr;
  }

  if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->m_keyboard) {
    FML_LOG(INFO) << "Keyboard Present";
    d->m_keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(d->m_keyboard, &keyboard_listener, d);
  } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->m_keyboard) {
    wl_keyboard_release(d->m_keyboard);
    d->m_keyboard = nullptr;
  }

  if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->m_touch.touch) {
    FML_LOG(INFO) << "Touch Present";
    d->m_touch.touch = wl_seat_get_touch(seat);
    wl_touch_set_user_data(d->m_touch.touch, d);
    wl_touch_add_listener(d->m_touch.touch, &touch_listener, d);
  } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && d->m_touch.touch) {
    wl_touch_release(d->m_touch.touch);
    d->m_touch.touch = nullptr;
  }
}

void Display::seat_handle_name(void* data,
                               struct wl_seat* seat,
                               const char* name) {
  (void)data;
  (void)seat;
  FML_DLOG(INFO) << "Seat: " << name;
}

const struct wl_seat_listener Display::seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

bool Display::pointerButtonStatePressed(struct pointer* p) {
  return (p->buttons) && (p->event.state == WL_POINTER_BUTTON_STATE_PRESSED);
}

void Display::pointer_handle_enter(void* data,
                                   struct wl_pointer* pointer,
                                   uint32_t serial,
                                   struct wl_surface* surface,
                                   wl_fixed_t sx,
                                   wl_fixed_t sy) {
  (void)pointer;
  auto* d = static_cast<Display*>(data);
  d->m_active_surface = surface;
  d->m_active_engine = d->m_surface_engine_map[surface];

  d->m_pointer.event.surface_x = wl_fixed_to_double(sx);
  d->m_pointer.event.surface_y = wl_fixed_to_double(sy);
  d->m_pointer.serial = serial;

  if (d->m_active_engine) {
    d->m_active_engine->SendMouseEvent(
        kFlutterPointerSignalKindNone, FlutterPointerPhase::kAdd,
        d->m_pointer.event.surface_x, d->m_pointer.event.surface_y, 0.0, 0.0,
        d->m_pointer.buttons);
  }
}

void Display::pointer_handle_leave(void* data,
                                   struct wl_pointer* pointer,
                                   uint32_t serial,
                                   struct wl_surface* surface) {
  (void)pointer;
  (void)surface;
  auto* d = static_cast<Display*>(data);

  d->m_pointer.serial = serial;

  if (d->m_active_engine) {
    d->m_active_engine->SendMouseEvent(kFlutterPointerSignalKindNone,
                                       FlutterPointerPhase::kRemove, 0.0, 0.0,
                                       0.0, 0.0, d->m_pointer.buttons);
  }
}

void Display::pointer_handle_motion(void* data,
                                    struct wl_pointer* pointer,
                                    uint32_t time,
                                    wl_fixed_t sx,
                                    wl_fixed_t sy) {
  (void)pointer;
  (void)time;
  auto* d = static_cast<Display*>(data);

  d->m_pointer.event.surface_x = wl_fixed_to_double(sx);
  d->m_pointer.event.surface_y = wl_fixed_to_double(sy);

  if (d->m_active_engine) {
    FlutterPointerPhase phase =
        pointerButtonStatePressed(&d->m_pointer) ? kMove : kHover;
    d->m_active_engine->SendMouseEvent(
        kFlutterPointerSignalKindNone, phase, d->m_pointer.event.surface_x,
        d->m_pointer.event.surface_y, 0.0, 0.0, d->m_pointer.buttons);
  }
}

void Display::pointer_handle_button(void* data,
                                    struct wl_pointer* wl_pointer,
                                    uint32_t serial,
                                    uint32_t time,
                                    uint32_t button,
                                    uint32_t state) {
  (void)wl_pointer;
  (void)time;
  auto* d = static_cast<Display*>(data);

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
    d->m_active_engine->SendMouseEvent(
        kFlutterPointerSignalKindNone, phase, d->m_pointer.event.surface_x,
        d->m_pointer.event.surface_y, 0.0, 0.0, d->m_pointer.buttons);
  }
}

void Display::pointer_handle_axis(void* data,
                                  struct wl_pointer* wl_pointer,
                                  uint32_t time,
                                  uint32_t axis,
                                  wl_fixed_t value) {
  (void)wl_pointer;
  auto* d = static_cast<Display*>(data);
  d->m_pointer.event.time = time;
  d->m_pointer.event.axes[axis].value = wl_fixed_to_double(value);

  if (d->m_active_engine) {
    d->m_active_engine->SendMouseEvent(
        kFlutterPointerSignalKindScroll, FlutterPointerPhase::kMove,
        d->m_pointer.event.surface_x, d->m_pointer.event.surface_y,
        d->m_pointer.event.axes[1].value, d->m_pointer.event.axes[0].value,
        d->m_pointer.buttons);
  }
}

void Display::pointer_handle_frame(void* data, struct wl_pointer* wl_pointer) {
  (void)data;
  (void)wl_pointer;
}

void Display::pointer_handle_axis_source(void* data,
                                         struct wl_pointer* wl_pointer,
                                         uint32_t axis_source) {
  (void)data;
  (void)wl_pointer;
  (void)axis_source;
}

void Display::pointer_handle_axis_stop(void* data,
                                       struct wl_pointer* wl_pointer,
                                       uint32_t time,
                                       uint32_t axis) {
  (void)data;
  (void)wl_pointer;
  (void)time;
  (void)axis;
}

void Display::pointer_handle_axis_discrete(void* data,
                                           struct wl_pointer* wl_pointer,
                                           uint32_t axis,
                                           int32_t discrete) {
  (void)data;
  (void)wl_pointer;
  (void)axis;
  (void)discrete;
}

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
                                    struct wl_keyboard* keyboard,
                                    uint32_t serial,
                                    struct wl_surface* surface,
                                    struct wl_array* keys) {
  (void)keyboard;
  (void)serial;
  (void)keys;
  auto* d = static_cast<Display*>(data);
  d->m_active_surface = surface;
  d->m_active_engine = d->m_surface_engine_map[surface];
}

void Display::keyboard_handle_leave(void* data,
                                    struct wl_keyboard* keyboard,
                                    uint32_t serial,
                                    struct wl_surface* surface) {
  (void)keyboard;
  (void)serial;
  (void)surface;

  auto* d = static_cast<Display*>(data);

  d->m_repeat_timer->disarm();
}

void Display::keyboard_handle_keymap(void* data,
                                     struct wl_keyboard* keyboard,
                                     uint32_t format,
                                     int fd,
                                     uint32_t size) {
  (void)keyboard;
  (void)format;
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
                                  struct wl_keyboard* keyboard,
                                  uint32_t serial,
                                  uint32_t time,
                                  uint32_t key,
                                  uint32_t state) {
  (void)keyboard;
  (void)serial;
  (void)time;
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
    const xkb_keysym_t* keysyms;
    int res = xkb_state_key_get_syms(d->m_xkb_state, xkb_scancode, &keysyms);
    if (res == 0) {
      FML_LOG(INFO) << "xkb_scancode has no keysyms: "
                    << "0x" << std::hex << xkb_scancode;
      keysym = XKB_KEY_NoSymbol;
    } else {
      // only use the first symbol until the use case for two is clarified
      keysym = keysyms[0];
      for (int i = 0; i < res; i++) {
        FML_LOG(INFO) << "xkb keysym: " << std::hex << "0x" << keysyms;
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
      FML_DLOG(INFO) << "key does not repeat: " << std::hex << "0x"
                     << xkb_scancode;
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
                                        struct wl_keyboard* keyboard,
                                        uint32_t serial,
                                        uint32_t mods_depressed,
                                        uint32_t mods_latched,
                                        uint32_t mods_locked,
                                        uint32_t group) {
  (void)keyboard;
  (void)serial;
  auto* d = static_cast<Display*>(data);
  xkb_state_update_mask(d->m_xkb_state, mods_depressed, mods_latched,
                        mods_locked, 0, 0, group);
}

void Display::keyboard_handle_repeat_info(void* data,
                                          struct wl_keyboard* wl_keyboard,
                                          int32_t rate,
                                          int32_t delay) {
  (void)wl_keyboard;

  auto d = reinterpret_cast<Display*>(data);

  d->m_repeat_timer->set_timerspec(rate, delay);

  FML_DLOG(INFO) << "[keyboard repeat info] rate: " << rate
                 << ", delay: " << delay;
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
                                struct wl_touch* wl_touch,
                                uint32_t serial,
                                uint32_t time,
                                struct wl_surface* surface,
                                int32_t id,
                                wl_fixed_t x_w,
                                wl_fixed_t y_w) {
  (void)wl_touch;
  (void)serial;
  (void)time;
  auto* d = static_cast<Display*>(data);

  bool first_down = (d->m_touch.down_count[id] == 0);
  d->m_touch.down_count[id] += 1;

  d->m_touch.surface_x = x_w;
  d->m_touch.surface_y = y_w;

  d->m_active_surface = surface;
  d->m_touch_engine = d->m_surface_engine_map[surface];
  if (d->m_touch_engine) {
    d->m_touch_engine->SendTouchEvent(
        (first_down ? FlutterPointerPhase::kDown : FlutterPointerPhase::kMove),
        wl_fixed_to_double(x_w), wl_fixed_to_double(y_w), id);
  }
}

void Display::touch_handle_up(void* data,
                              struct wl_touch* wl_touch,
                              uint32_t serial,
                              uint32_t time,
                              int32_t id) {
  (void)wl_touch;
  (void)serial;
  (void)time;

  auto* d = static_cast<Display*>(data);

  d->m_touch.down_count[id] -= 1;

  if (d->m_touch_engine) {
    bool last_up = (d->m_touch.down_count[id] == 0);
    d->m_touch_engine->SendTouchEvent(
        (last_up ? FlutterPointerPhase::kUp : FlutterPointerPhase::kMove),
        wl_fixed_to_double(d->m_touch.surface_x),
        wl_fixed_to_double(d->m_touch.surface_y), id);
  }
}

void Display::touch_handle_motion(void* data,
                                  struct wl_touch* wl_touch,
                                  uint32_t time,
                                  int32_t id,
                                  wl_fixed_t x_w,
                                  wl_fixed_t y_w) {
  (void)wl_touch;
  (void)time;
  auto* d = static_cast<Display*>(data);

  d->m_touch.surface_x = x_w;
  d->m_touch.surface_y = y_w;

  if (d->m_touch_engine) {
    d->m_touch_engine->SendTouchEvent(FlutterPointerPhase::kMove,
                                      wl_fixed_to_double(x_w),
                                      wl_fixed_to_double(y_w), id);
  }
}

void Display::touch_handle_cancel(void* data, struct wl_touch* wl_touch) {
  (void)wl_touch;
  auto* d = static_cast<Display*>(data);
  if (d->m_touch_engine) {
    d->m_touch_engine->SendTouchEvent(FlutterPointerPhase::kCancel,
                                      d->m_pointer.event.surface_x,
                                      d->m_pointer.event.surface_y, 0);
  }
}

void Display::touch_handle_frame(void* data, struct wl_touch* wl_touch) {
  (void)data;
  (void)wl_touch;
}

const struct wl_touch_listener Display::touch_listener = {
    .down = touch_handle_down,
    .up = touch_handle_up,
    .motion = touch_handle_motion,
    .frame = touch_handle_frame,
    .cancel = touch_handle_cancel,
};

int Display::PollEvents() {
  while (wl_display_prepare_read(m_display) != 0) {
    wl_display_dispatch_pending(m_display);
  }
  wl_display_flush(m_display);

  wl_display_read_events(m_display);
  return wl_display_dispatch_pending(m_display);
}

void Display::AglShellDoBackground(struct wl_surface* surface, size_t index) {
  if (m_agl.shell) {
    agl_shell_set_background(m_agl.shell, surface,
                             m_all_outputs[index]->output);
  }
}

void Display::AglShellDoPanel(struct wl_surface* surface,
                              enum agl_shell_edge mode,
                              size_t index) {
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

void Display::AglShellDoSetupActivationArea(uint32_t x,
                                            uint32_t y,
                                            uint32_t index) {
  uint32_t width = m_all_outputs[index]->width;
  uint32_t height = m_all_outputs[index]->height - (2 * y);

  if (!m_agl.shell)
    return;

  if (m_all_outputs[index]->transform == WL_OUTPUT_TRANSFORM_90) {
    width = m_all_outputs[index]->height;
    height = m_all_outputs[index]->width - (2 * y);
  }

  FML_LOG(INFO) << "Using custom rectangle [" << width << "x" << height << "+"
                << x << "x" << y << "] for activation";

  agl_shell_set_activate_region(m_agl.shell, m_all_outputs[index]->output, x, y,
                                width, height);
}

void Display::SetEngine(wl_surface* surface, Engine* engine) {
  m_active_engine = engine;
  m_active_surface = surface;
  m_surface_engine_map[surface] = engine;
}

bool Display::ActivateSystemCursor(int32_t device, const std::string& kind) {
  (void)device;
  if (!m_enable_cursor) {
    wl_pointer_set_cursor(m_pointer.pointer, m_pointer.serial, m_cursor_surface,
                          0, 0);
    wl_surface_damage(m_cursor_surface, 0, 0, 0, 0);
    wl_surface_commit(m_cursor_surface);
    return true;
  }

  if (m_pointer.pointer) {
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
      FML_DLOG(INFO) << "Cursor Kind = " << kind;
      return false;
    }

    auto cursor = wl_cursor_theme_get_cursor(m_cursor_theme, cursor_name);
    if (cursor == nullptr) {
      FML_DLOG(INFO) << "Cursor [" << cursor_name << "] not found";
      return false;
    }
    auto cursor_buffer = wl_cursor_image_get_buffer(cursor->images[0]);
    if (cursor_buffer && m_cursor_surface) {
      wl_pointer_set_cursor(m_pointer.pointer, m_pointer.serial,
                            m_cursor_surface,
                            (int32_t)cursor->images[0]->hotspot_x,
                            (int32_t)cursor->images[0]->hotspot_y);
      wl_surface_attach(m_cursor_surface, cursor_buffer, 0, 0);
      wl_surface_damage(m_cursor_surface, 0, 0,
                        (int32_t)cursor->images[0]->width,
                        (int32_t)cursor->images[0]->height);
      wl_surface_commit(m_cursor_surface);
    } else {
      FML_DLOG(INFO) << "Failed to set cursor: Invalid Cursor Buffer";
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

int32_t Display::GetBufferScale(uint32_t index) {
  if (index < m_all_outputs.size()) {
    if (m_buffer_scale_enable) {
      return m_all_outputs[index]->scale;
    } else {
      return (int32_t)kDefaultBufferScale;
    }
  }
  FML_DLOG(ERROR) << "Invalid output index: " << index;
  return (int32_t)kDefaultBufferScale;
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

void Display::agl_shell_app_state(void* data,
                                  struct agl_shell* agl_shell,
                                  const char* app_id,
                                  uint32_t state) {
  auto* d = static_cast<Display*>(data);

  switch (state) {
    case AGL_SHELL_APP_STATE_STARTED:
      FML_DLOG(INFO) << "Got AGL_SHELL_APP_STATE_STARTED for app_id " << app_id;

      if (d->m_agl.shell) {
	      // we always assume the first output advertised by the wl_output
	      // interface
	      unsigned int default_output_index = 0;

	      agl_shell_activate_app(d->m_agl.shell, app_id,
			             d->m_all_outputs[default_output_index]->output);
      }

      break;
    case AGL_SHELL_APP_STATE_TERMINATED:
      FML_DLOG(INFO) << "Got AGL_SHELL_APP_STATE_TERMINATED for app_id "
                     << app_id;
      break;
    case AGL_SHELL_APP_STATE_ACTIVATED:
      FML_DLOG(INFO) << "Got AGL_SHELL_APP_STATE_ACTIVATED for app_id "
                     << app_id;
      break;
    default:
      break;
  }
}

const struct agl_shell_listener Display::agl_shell_listener = {
    .bound_ok = agl_shell_bound_ok,
    .bound_fail = agl_shell_bound_fail,
    .app_state = agl_shell_app_state,
};
