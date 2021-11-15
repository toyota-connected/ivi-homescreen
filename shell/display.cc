// Copyright 2020 Toyota Connected North America
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

#include <flutter/fml/logging.h>
#include <unistd.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <utility>

#include "app.h"
#include "constants.h"
#include "engine.h"

Display::Display([[maybe_unused]] App* app)
    : m_flutter_engine(nullptr),
      m_display(nullptr),
      m_output(nullptr),
      m_compositor(nullptr),
      m_keyboard(nullptr),
      m_agl_shell(nullptr),
      m_has_xrgb(false) {
  FML_DLOG(INFO) << "+ Display()";

  m_display = wl_display_connect(nullptr);
  if (m_display == nullptr) {
    FML_LOG(ERROR) << "Failed to connect to Wayland display. "
                   << strerror(errno);
    exit(-1);
  }

  m_registry = wl_display_get_registry(m_display);
  wl_registry_add_listener(m_registry, &registry_listener, this);
  wl_display_dispatch(m_display);

  if (!m_compositor) {
    assert(false);
  }

  if (!m_display) {
    assert(false);
  }

  if (!m_shm) {
    FML_LOG(ERROR) << "No wl_shm global";
    assert(false);
  }

  if (!m_output) {
    assert(false);
  }

  if (!m_agl_shell) {
    FML_LOG(INFO) << "No agl_shell extension present";
  }

  if (!m_has_xrgb) {
    FML_LOG(INFO) << "WL_SHM_FORMAT_XRGB32 not available";
  }

  FML_DLOG(INFO) << "- Display()";
}

Display::~Display() {
  FML_DLOG(INFO) << "+ ~Display()";

  if (m_shm)
    wl_shm_destroy(m_shm);

  if (m_agl_shell)
    agl_shell_destroy(m_agl_shell);

  if (m_compositor)
    wl_compositor_destroy(m_compositor);

  wl_registry_destroy(m_registry);
  wl_display_flush(m_display);
  wl_display_disconnect(m_display);

  FML_DLOG(INFO) << "- ~Display()";
}

void Display::registry_handle_global(void* data,
                                     struct wl_registry* registry,
                                     uint32_t name,
                                     const char* interface,
                                     [[maybe_unused]] uint32_t version) {
  auto* d = static_cast<Display*>(data);

  FML_DLOG(INFO) << "Wayland: " << interface;

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    d->m_compositor = static_cast<struct wl_compositor*>(
        wl_registry_bind(registry, name, &wl_compositor_interface, 1));
  }

  if (strcmp(interface, wl_shell_interface.name) == 0) {
    d->m_shell = static_cast<struct wl_shell*>(
        wl_registry_bind(registry, name, &wl_shell_interface, 1));
  }
  else if (strcmp(interface, wl_shm_interface.name) == 0) {
    d->m_shm = static_cast<struct wl_shm*>(
        wl_registry_bind(registry, name, &wl_shm_interface, 1));

    d->m_cursor_theme = wl_cursor_theme_load(nullptr, 32, d->m_shm);
    d->m_default_cursor =
        wl_cursor_theme_get_cursor(d->m_cursor_theme, "left_ptr");

    wl_shm_add_listener(d->m_shm, &shm_listener, d);
  }

  else if (strcmp(interface, wl_output_interface.name) == 0) {
    d->m_output = static_cast<struct wl_output*>(
        wl_registry_bind(registry, name, &wl_output_interface, 1));
    wl_output_add_listener(d->m_output, &output_listener, d);
  }

  else if (strcmp(interface, wl_seat_interface.name) == 0) {
    d->m_seat = static_cast<wl_seat*>(
        wl_registry_bind(registry, name, &wl_seat_interface, 1));
    wl_seat_add_listener(d->m_seat, &seat_listener, d);
  }

  else if (strcmp(interface, zwp_pointer_gestures_v1_interface.name) == 0) {
    d->m_gestures = static_cast<zwp_pointer_gestures_v1*>(wl_registry_bind(
        registry, name, &zwp_pointer_gestures_v1_interface, 1));
  }

  else if (strcmp(interface, agl_shell_interface.name) == 0) {
    d->m_agl_shell = static_cast<struct agl_shell*>(
        wl_registry_bind(registry, name, &agl_shell_interface, 1));
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
  auto* d = static_cast<Display*>(data);

  if (wl_output == d->m_output) {
    d->m_info.geometry.x = x;
    d->m_info.geometry.y = y;
    d->m_info.geometry.physical_width = physical_width;
    d->m_info.geometry.physical_height = physical_height;
    d->m_info.geometry.subpixel = subpixel;
    d->m_info.geometry.size = physical_width * physical_height;
    d->m_info.geometry.make = std::string(make);
    d->m_info.geometry.model = std::string(model);
    d->m_info.geometry.transform = transform;

    FML_DLOG(INFO) << "x: " << d->m_info.geometry.x;
    FML_DLOG(INFO) << "y: " << d->m_info.geometry.y;
    FML_DLOG(INFO) << "physical_width: " << d->m_info.geometry.physical_width;
    FML_DLOG(INFO) << "physical_height: " << d->m_info.geometry.physical_height;
    FML_DLOG(INFO) << "size: " << d->m_info.geometry.size;
    FML_DLOG(INFO) << "subpixel: " << d->m_info.geometry.subpixel;
    FML_DLOG(INFO) << "make: " << d->m_info.geometry.make;
    FML_DLOG(INFO) << "model: " << d->m_info.geometry.model;
    FML_DLOG(INFO) << "transform: " << d->m_info.geometry.transform;
  }
}

void Display::display_handle_mode(void* data,
                                  struct wl_output* wl_output,
                                  uint32_t flags,
                                  int width,
                                  int height,
                                  [[maybe_unused]] int refresh) {
  auto* d = static_cast<Display*>(data);

  if (wl_output == d->m_output && (flags & WL_OUTPUT_MODE_CURRENT)) {
    double dots = width * height;
    double dots_per_mm = dots / d->m_info.geometry.size;
    double dots_per_in = dots_per_mm / 0.155;

    d->m_info.mode.width = width;
    d->m_info.mode.height = height;
    d->m_info.mode.dots_per_in = dots_per_in;

    FML_DLOG(INFO) << "width: " << d->m_info.mode.width;
    FML_DLOG(INFO) << "height: " << d->m_info.mode.height;
    FML_DLOG(INFO) << "dpi: " << d->m_info.mode.dots_per_in;
  }
}

void Display::display_handle_scale(void* data,
                                   struct wl_output* wl_output,
                                   int scale) {
  auto* d = static_cast<Display*>(data);

  if (wl_output == d->m_output) {
    d->m_info.scale.scale = scale;

    FML_DLOG(INFO) << "scale: " << d->m_info.scale.scale;
  }
}

void Display::display_handle_done(void* data, struct wl_output* wl_output) {
  (void)data;
  (void)wl_output;
}

const struct wl_output_listener Display::output_listener = {
    display_handle_geometry, display_handle_mode, display_handle_done,
    display_handle_scale};

void Display::shm_format(void* data,
                         [[maybe_unused]] struct wl_shm* wl_shm,
                         uint32_t format) {
  auto* display = reinterpret_cast<Display*>(data);

  if (format == WL_SHM_FORMAT_XRGB8888)
    display->m_has_xrgb = true;
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

const struct wl_seat_listener Display::seat_listener = {
    .capabilities = seat_handle_capabilities,
};

FlutterPointerPhase Display::getPointerPhase(struct pointer* p) {
  FlutterPointerPhase res = p->phase;

  //  FML_DLOG(INFO) << "getPointerPhase: Button: " << p->event.button << ", "
  //  << "State: " << p->event.state;

  if (p->buttons) {
    switch (p->event.state) {
      case WL_POINTER_BUTTON_STATE_PRESSED:
        if (p->phase == FlutterPointerPhase::kDown)
          res = FlutterPointerPhase::kMove;
        else
          res = FlutterPointerPhase::kDown;
        break;
      case WL_POINTER_BUTTON_STATE_RELEASED:
        if ((p->phase == FlutterPointerPhase::kDown) ||
            (p->phase == FlutterPointerPhase::kMove))
          res = FlutterPointerPhase::kUp;
        p->buttons = 0;
        break;
    }
  } else {
    res = p->event.state == WL_POINTER_BUTTON_STATE_RELEASED
              ? FlutterPointerPhase::kHover
              : FlutterPointerPhase::kUp;
  }

  p->phase = res;
  return res;
}

void Display::pointer_handle_enter(void* data,
                                   [[maybe_unused]] struct wl_pointer* pointer,
                                   [[maybe_unused]] uint32_t serial,
                                   [[maybe_unused]] struct wl_surface* surface,
                                   wl_fixed_t sx,
                                   wl_fixed_t sy) {
  auto* d = static_cast<Display*>(data);

  d->m_pointer.event.surface_x = wl_fixed_to_double(sx);
  d->m_pointer.event.surface_y = wl_fixed_to_double(sy);

  if (d->m_flutter_engine) {
    d->m_flutter_engine->SendMouseEvent(
        kFlutterPointerSignalKindNone, FlutterPointerPhase::kAdd,
        d->m_pointer.event.surface_x, d->m_pointer.event.surface_y, 0.0, 0.0,
        d->m_pointer.buttons);
  }
}

void Display::pointer_handle_leave(
    void* data,
    [[maybe_unused]] struct wl_pointer* pointer,
    [[maybe_unused]] uint32_t serial,
    [[maybe_unused]] struct wl_surface* surface) {
  auto* d = static_cast<Display*>(data);

  if (d->m_flutter_engine) {
    d->m_flutter_engine->SendMouseEvent(kFlutterPointerSignalKindNone,
                                        FlutterPointerPhase::kRemove, 0.0, 0.0,
                                        0.0, 0.0, d->m_pointer.buttons);
  }
}

void Display::pointer_handle_motion(void* data,
                                    [[maybe_unused]] struct wl_pointer* pointer,
                                    [[maybe_unused]] uint32_t time,
                                    wl_fixed_t sx,
                                    wl_fixed_t sy) {
  auto* d = static_cast<Display*>(data);

  d->m_pointer.event.surface_x = wl_fixed_to_double(sx);
  d->m_pointer.event.surface_y = wl_fixed_to_double(sy);

  if (d->m_flutter_engine) {
    d->m_flutter_engine->SendMouseEvent(
        kFlutterPointerSignalKindNone, getPointerPhase(&d->m_pointer),
        d->m_pointer.event.surface_x, d->m_pointer.event.surface_y, 0.0, 0.0,
        d->m_pointer.buttons);
  }
}

void Display::pointer_handle_button(
    void* data,
    [[maybe_unused]] struct wl_pointer* wl_pointer,
    [[maybe_unused]] uint32_t serial,
    [[maybe_unused]] uint32_t time,
    uint32_t button,
    uint32_t state) {
  auto* d = static_cast<Display*>(data);

  d->m_pointer.event.state = state;
  d->m_pointer.buttons = d->m_pointer.event.button = button;

  if (d->m_flutter_engine) {
    d->m_flutter_engine->SendMouseEvent(
        kFlutterPointerSignalKindNone, getPointerPhase(&d->m_pointer),
        d->m_pointer.event.surface_x, d->m_pointer.event.surface_y, 0.0, 0.0,
        d->m_pointer.buttons);
  }
}

void Display::pointer_handle_axis(
    void* data,
    [[maybe_unused]] struct wl_pointer* wl_pointer,
    uint32_t time,
    uint32_t axis,
    wl_fixed_t value) {
  auto* d = static_cast<Display*>(data);
  d->m_pointer.event.time = time;
  d->m_pointer.event.axes[axis].value = wl_fixed_to_double(value);

  if (d->m_flutter_engine) {
    d->m_flutter_engine->SendMouseEvent(
        kFlutterPointerSignalKindScroll, getPointerPhase(&d->m_pointer),
        d->m_pointer.event.surface_x, d->m_pointer.event.surface_y,
        d->m_pointer.event.axes[1].value, d->m_pointer.event.axes[0].value,
        d->m_pointer.buttons);
  }
}

const struct wl_pointer_listener Display::pointer_listener = {
    .enter = pointer_handle_enter,
    .leave = pointer_handle_leave,
    .motion = pointer_handle_motion,
    .button = pointer_handle_button,
    .axis = pointer_handle_axis,
};

void Display::keyboard_handle_keymap(
    [[maybe_unused]] void* data,
    [[maybe_unused]] struct wl_keyboard* keyboard,
    uint32_t format,
    int fd,
    uint32_t size) {
  FML_DLOG(INFO) << "keyboard keymap format " << format << " fd " << fd
                 << " size " << size;
  // Just so we don’t leak the keymap fd
  close(fd);
}

void Display::keyboard_handle_enter(
    [[maybe_unused]] void* data,
    [[maybe_unused]] struct wl_keyboard* keyboard,
    [[maybe_unused]] uint32_t serial,
    [[maybe_unused]] struct wl_surface* surface,
    [[maybe_unused]] struct wl_array* keys) {
  FML_DLOG(INFO) << "keyboard enter";
}

void Display::keyboard_handle_leave(
    [[maybe_unused]] void* data,
    [[maybe_unused]] struct wl_keyboard* keyboard,
    [[maybe_unused]] uint32_t serial,
    [[maybe_unused]] struct wl_surface* surface) {
  FML_DLOG(INFO) << "keyboard leave";
}

void Display::keyboard_handle_key([[maybe_unused]] void* data,
                                  [[maybe_unused]] struct wl_keyboard* keyboard,
                                  [[maybe_unused]] uint32_t serial,
                                  [[maybe_unused]] uint32_t time,
                                  uint32_t key,
                                  uint32_t state) {
  FML_DLOG(INFO) << "keyboard key " << key << " state " << state;
}

void Display::keyboard_handle_modifiers(
    [[maybe_unused]] void* data,
    [[maybe_unused]] struct wl_keyboard* keyboard,
    [[maybe_unused]] uint32_t serial,
    uint32_t mods_depressed,
    uint32_t mods_latched,
    uint32_t mods_locked,
    uint32_t group) {
  FML_DLOG(INFO) << "keyboard modifiers: mods_depressed " << mods_depressed
                 << " mods_latched " << mods_latched << " mods_locked "
                 << mods_locked << " group " << group;
}

const struct wl_keyboard_listener Display::keyboard_listener = {
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .key = keyboard_handle_key,
    .modifiers = keyboard_handle_modifiers,
};

[[maybe_unused]]
struct Display::touch_point* Display::get_touch_point(Display* d, int32_t id) {
  struct touch_event* touch = &d->m_touch.event;

  for (auto& point : touch->points) {
    if (point.id == id && point.valid) {
      return &point;
    }
    if (!point.valid) {
      point.valid = true;
      point.id = id;
      return &point;
    }
  }
  return nullptr;
}

void Display::touch_handle_down(void* data,
                                [[maybe_unused]] struct wl_touch* wl_touch,
                                [[maybe_unused]] uint32_t serial,
                                [[maybe_unused]] uint32_t time,
                                [[maybe_unused]] struct wl_surface* surface,
                                int32_t id,
                                wl_fixed_t x_w,
                                wl_fixed_t y_w) {
  auto* d = static_cast<Display*>(data);

  bool first_down = (d->m_touch.down_count[id] == 0);
  d->m_touch.down_count[id] += 1;

  d->m_touch.surface_x = x_w;
  d->m_touch.surface_y = y_w;

  if (d->m_flutter_engine) {
    d->m_flutter_engine->SendTouchEvent(
        (first_down ? FlutterPointerPhase::kDown : FlutterPointerPhase::kMove),
        wl_fixed_to_double(x_w), wl_fixed_to_double(y_w), id);
  }
}

void Display::touch_handle_up(void* data,
                              [[maybe_unused]] struct wl_touch* wl_touch,
                              [[maybe_unused]] uint32_t serial,
                              [[maybe_unused]] uint32_t time,
                              int32_t id) {
  auto* d = static_cast<Display*>(data);

  d->m_touch.down_count[id] -= 1;
  bool last_up = (d->m_touch.down_count[id] == 0);

  if (d->m_flutter_engine) {
    d->m_flutter_engine->SendTouchEvent(
        (last_up ? FlutterPointerPhase::kUp : FlutterPointerPhase::kMove),
        wl_fixed_to_double(d->m_touch.surface_x),
        wl_fixed_to_double(d->m_touch.surface_y), id);
  }
}

void Display::touch_handle_motion(void* data,
                                  [[maybe_unused]] struct wl_touch* wl_touch,
                                  [[maybe_unused]] uint32_t time,
                                  int32_t id,
                                  wl_fixed_t x_w,
                                  wl_fixed_t y_w) {
  auto* d = static_cast<Display*>(data);

  d->m_touch.surface_x = x_w;
  d->m_touch.surface_y = y_w;

  if (d->m_flutter_engine) {
    d->m_flutter_engine->SendTouchEvent(FlutterPointerPhase::kMove,
                                        wl_fixed_to_double(x_w),
                                        wl_fixed_to_double(y_w), id);
  }
}

void Display::touch_handle_cancel(void* data,
                                  [[maybe_unused]] struct wl_touch* wl_touch) {
  auto* d = static_cast<Display*>(data);
  if (d->m_flutter_engine) {
    d->m_flutter_engine->SendTouchEvent(FlutterPointerPhase::kCancel,
                                        d->m_pointer.event.surface_x,
                                        d->m_pointer.event.surface_y, 0);
  }
}

void Display::touch_handle_frame([[maybe_unused]] void* data,
                                 [[maybe_unused]] struct wl_touch* wl_touch) {
  FML_DLOG(INFO) << "touch_handle_frame";
}

const struct wl_touch_listener Display::touch_listener = {
    .down = touch_handle_down,
    .up = touch_handle_up,
    .motion = touch_handle_motion,
    .frame = touch_handle_frame,
    .cancel = touch_handle_cancel,
};

void Display::gesture_pinch_begin(
    [[maybe_unused]] void* data,
    [[maybe_unused]] struct zwp_pointer_gesture_pinch_v1* zwp_pointer_gesture_pinch_v1,
    [[maybe_unused]] uint32_t serial,
    [[maybe_unused]] uint32_t time,
    [[maybe_unused]] struct wl_surface* surface,
    uint32_t fingers) {
  FML_DLOG(INFO) << "gesture_pinch_begin: fingers: " << fingers;
}

void Display::gesture_pinch_update(
    [[maybe_unused]] void* data,
    [[maybe_unused]] struct zwp_pointer_gesture_pinch_v1* zwp_pointer_gesture_pinch_v1,
    [[maybe_unused]] uint32_t time,
    wl_fixed_t dx,
    wl_fixed_t dy,
    wl_fixed_t scale,
    wl_fixed_t rotation) {
  FML_DLOG(INFO) << "gesture_pinch_update: " << wl_fixed_to_double(dx) << ", "
                 << wl_fixed_to_double(dy)
                 << " scale: " << wl_fixed_to_double(scale)
                 << ", rotation: " << wl_fixed_to_double(rotation);
}

void Display::gesture_pinch_end(
    [[maybe_unused]] void* data,
    [[maybe_unused]] struct zwp_pointer_gesture_pinch_v1* zwp_pointer_gesture_pinch_v1,
    [[maybe_unused]] uint32_t serial,
    [[maybe_unused]] uint32_t time,
    int32_t cancelled) {
  FML_DLOG(INFO) << "gesture_pinch_end";
  FML_DLOG(INFO) << "gesture_pinch_begin: cancelled: " << cancelled;
}

const struct zwp_pointer_gesture_pinch_v1_listener
    Display::gesture_pinch_listener = {
        .begin = gesture_pinch_begin,
        .update = gesture_pinch_update,
        .end = gesture_pinch_end,
};

[[maybe_unused]] void Display::AglShellDoBackground(
    struct wl_surface* surface) {
  if (m_agl_shell) {
    agl_shell_set_background(m_agl_shell, surface, m_output);
  }
}

[[maybe_unused]] void Display::AglShellDoPanel(struct wl_surface* surface,
                                               enum agl_shell_edge mode) {
  if (m_agl_shell) {
    agl_shell_set_panel(m_agl_shell, surface, m_output, mode);
  }
}

[[maybe_unused]] void Display::AglShellDoReady() {
  if (m_agl_shell) {
    agl_shell_ready(m_agl_shell);
  }
}

void Display::SetEngine(std::shared_ptr<Engine> engine) {
  m_flutter_engine = std::move(engine);
}
