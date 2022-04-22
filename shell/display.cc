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

#include <algorithm>
#include <flutter/fml/logging.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <utility>

#include "app.h"
#include "constants.h"
#include "engine.h"

Display::Display([[maybe_unused]] App* app,
                 bool enable_cursor,
                 std::string cursor_theme_name)
    : m_flutter_engine(nullptr),
      m_display(nullptr),
      m_compositor(nullptr),
      m_subcompositor(nullptr),
      m_cursor_surface(nullptr),
      m_keyboard(nullptr),
      m_xkb_context(xkb_context_new(XKB_CONTEXT_NO_FLAGS)),
      m_keymap(nullptr),
      m_xkb_state(nullptr),
      m_buffer_scale(1),
      m_last_buffer_scale(1),
	  m_agl_shell(nullptr),
      m_enable_cursor(enable_cursor),
      m_cursor_theme_name(std::move(cursor_theme_name)) {
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

  if (!m_subcompositor) {
    assert(false);
  }

  if (!m_display) {
    assert(false);
  }

  if (!m_shm) {
    FML_LOG(ERROR) << "No wl_shm global";
    assert(false);
  }

  if (!m_agl_shell) {
    FML_LOG(INFO) << "No agl_shell extension present";
  }

  FML_DLOG(INFO) << "- Display()";
}

Display::~Display() {
  FML_DLOG(INFO) << "+ ~Display()";

  if (m_shm)
    wl_shm_destroy(m_shm);

  if (m_agl_shell)
    agl_shell_destroy(m_agl_shell);

  if (m_subcompositor)
    wl_subcompositor_destroy(m_subcompositor);

  if (m_compositor)
    wl_compositor_destroy(m_compositor);

  if (m_cursor_theme)
    wl_cursor_theme_destroy(m_cursor_theme);

  if (m_cursor_surface)
    wl_surface_destroy(m_cursor_surface);

  wl_registry_destroy(m_registry);
  wl_display_flush(m_display);
  wl_display_disconnect(m_display);

  FML_DLOG(INFO) << "- ~Display()";
}

void Display::registry_handle_global(
    void* data,
    [[maybe_unused]] struct wl_registry* registry,
    [[maybe_unused]] uint32_t name,
    [[maybe_unused]] const char* interface,
    [[maybe_unused]] uint32_t version) {
  auto* d = static_cast<Display*>(data);

  FML_DLOG(INFO) << "Wayland: " << interface;

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    d->m_compositor = static_cast<struct wl_compositor*>(
        wl_registry_bind(registry, name, &wl_compositor_interface, 1));
  }

  else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
    d->m_subcompositor = static_cast<struct wl_subcompositor*>(
        wl_registry_bind(registry, name, &wl_subcompositor_interface, 1));
  }

  if (strcmp(interface, wl_shell_interface.name) == 0) {
    d->m_shell = static_cast<struct wl_shell*>(
        wl_registry_bind(registry, name, &wl_shell_interface, 1));
  }
  
  else if (strcmp(interface, wl_shm_interface.name) == 0) {
    d->m_shm = static_cast<struct wl_shm*>(
        wl_registry_bind(registry, name, &wl_shm_interface, 1));

    if (d->m_enable_cursor) {
      d->m_cursor_theme = wl_cursor_theme_load(d->m_cursor_theme_name.c_str(),
                                               kCursorSize, d->m_shm);
      d->m_cursor_surface = wl_compositor_create_surface(d->m_compositor);
    }

    wl_shm_add_listener(d->m_shm, &shm_listener, d);
  }

  else if (strcmp(interface, wl_output_interface.name) == 0) {
    auto oi = std::make_shared<output_info_t>();
    std::fill_n(oi.get(), 1, output_info_t{});
    oi->global_id = name;
    oi->output = static_cast<struct wl_output*>(wl_registry_bind(
        registry, name, &wl_output_interface,
        std::min(static_cast<uint32_t>(2), version)));
    wl_output_add_listener(oi->output, &output_listener, oi.get());
    d->m_all_outputs.push_back(oi);

    d->m_is_configured = false;
    wl_callback* callback = wl_display_sync(d->GetDisplay());
    wl_callback_add_listener(callback, &configure_callback_listener, d);
  }

  else if (strcmp(interface, wl_seat_interface.name) == 0) {
    d->m_seat = static_cast<wl_seat*>(
        wl_registry_bind(registry, name, &wl_seat_interface, 1));
    wl_seat_add_listener(d->m_seat, &seat_listener, d);
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
  (void)data;
  (void)wl_output;
  (void)x;
  (void)y;
  (void)subpixel;
  (void)make;
  (void)model;
  (void)transform;

  auto* oi = static_cast<output_info_t*>(data);
  oi->physical_width = physical_width;
  oi->physical_height = physical_height;

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
  oi->width = width;
  oi->height = height;
  oi->refresh_rate = refresh;

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

void Display::wl_output_configure_callback(void* data,
                                           wl_callback* callback,
                                           [[maybe_unused]] uint32_t time) {
  auto* display = static_cast<Display*>(data);

  wl_callback_destroy(callback);

  display->m_is_configured = true;
}

const struct wl_callback_listener Display::configure_callback_listener = {
    .done = wl_output_configure_callback,
};

void Display::shm_format(void* data,
                         [[maybe_unused]] struct wl_shm* wl_shm,
                         [[maybe_unused]] uint32_t format) {
  (void)data;
  (void)wl_shm;
  (void)format;
}

const struct wl_shm_listener Display::shm_listener = {shm_format};

void Display::seat_handle_capabilities(void* data,
                                       struct wl_seat* seat,
                                       [[maybe_unused]] uint32_t caps) {
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
  [[maybe_unused]] FlutterPointerPhase res = p->phase;

  if (p->buttons) {
    switch (p->event.state) {
      case WL_POINTER_BUTTON_STATE_PRESSED:
        // FML_DLOG(INFO) << "WL_POINTER_BUTTON_STATE_PRESSED";
        if ((p->phase == FlutterPointerPhase::kHover) ||
            (p->phase == FlutterPointerPhase::kUp))
          res = FlutterPointerPhase::kDown;
        else
          res = FlutterPointerPhase::kMove;
      case WL_POINTER_BUTTON_STATE_RELEASED:
        // FML_DLOG(INFO) << "WL_POINTER_BUTTON_STATE_RELEASED";
        if ((p->phase == FlutterPointerPhase::kDown) ||
            (p->phase == FlutterPointerPhase::kMove))
          res = FlutterPointerPhase::kUp;
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
                                   uint32_t serial,
                                   [[maybe_unused]] struct wl_surface* surface,
                                   [[maybe_unused]] wl_fixed_t sx,
                                   [[maybe_unused]] wl_fixed_t sy) {
  auto* d = static_cast<Display*>(data);

  d->m_pointer.event.surface_x = wl_fixed_to_double(sx);
  d->m_pointer.event.surface_y = wl_fixed_to_double(sy);
  d->m_pointer.serial = serial;

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
    uint32_t serial,
    [[maybe_unused]] struct wl_surface* surface) {
  auto* d = static_cast<Display*>(data);

  d->m_pointer.serial = serial;

  if (d->m_flutter_engine) {
    d->m_flutter_engine->SendMouseEvent(kFlutterPointerSignalKindNone,
                                        FlutterPointerPhase::kRemove, 0.0, 0.0,
                                        0.0, 0.0, d->m_pointer.buttons);
  }
}

void Display::pointer_handle_motion(void* data,
                                    [[maybe_unused]] struct wl_pointer* pointer,
                                    [[maybe_unused]] uint32_t time,
                                    [[maybe_unused]] wl_fixed_t sx,
                                    [[maybe_unused]] wl_fixed_t sy) {
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
    uint32_t serial,
    [[maybe_unused]] uint32_t time,
    uint32_t button,
    uint32_t state) {
  auto* d = static_cast<Display*>(data);

  d->m_pointer.event.state = state;
  d->m_pointer.buttons = d->m_pointer.event.button = button;
  d->m_pointer.serial = serial;

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
    [[maybe_unused]] uint32_t axis,
    [[maybe_unused]] wl_fixed_t value) {
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

void Display::keyboard_handle_enter(
    [[maybe_unused]] void* data,
    [[maybe_unused]] struct wl_keyboard* keyboard,
    [[maybe_unused]] uint32_t serial,
    [[maybe_unused]] struct wl_surface* surface,
    [[maybe_unused]] struct wl_array* keys) {}

void Display::keyboard_handle_leave(
    [[maybe_unused]] void* data,
    [[maybe_unused]] struct wl_keyboard* keyboard,
    [[maybe_unused]] uint32_t serial,
    [[maybe_unused]] struct wl_surface* surface) {}

void Display::keyboard_handle_keymap(
    void* data,
    [[maybe_unused]] struct wl_keyboard* keyboard,
    [[maybe_unused]] uint32_t format,
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

void Display::keyboard_handle_key([[maybe_unused]] void* data,
                                  [[maybe_unused]] struct wl_keyboard* keyboard,
                                  [[maybe_unused]] uint32_t serial,
                                  [[maybe_unused]] uint32_t time,
                                  [[maybe_unused]] uint32_t key,
                                  [[maybe_unused]] uint32_t state) {
#if ENABLE_PLUGIN_TEXT_INPUT
  auto* d = static_cast<Display*>(data);
  if (d->m_text_input) {
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
      xkb_keysym_t keysym = xkb_state_key_get_one_sym(d->m_xkb_state, key + 8);
      d->m_text_input->keyboard_handle_key(d->m_text_input.get(), keyboard,
                                           serial, time, keysym, state);
    }
  }
#else
#if !defined(NDEBUG)
  if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    [[maybe_unused]] xkb_keysym_t keysym =
        xkb_state_key_get_one_sym(d->m_xkb_state, key + 8);
    uint32_t utf32 = xkb_keysym_to_utf32(keysym);
    if (utf32) {
      FML_DLOG(INFO) << "[Press] U" << utf32;
    } else {
      [[maybe_unused]] char name[64];
      xkb_keysym_get_name(keysym, name, 64);
      FML_DLOG(INFO) << "[Press] " << name;
    }
  }
#endif
#endif
}

void Display::keyboard_handle_modifiers(
    void* data,
    [[maybe_unused]] struct wl_keyboard* keyboard,
    [[maybe_unused]] uint32_t serial,
    uint32_t mods_depressed,
    uint32_t mods_latched,
    uint32_t mods_locked,
    uint32_t group) {
  auto* d = static_cast<Display*>(data);
  xkb_state_update_mask(d->m_xkb_state, mods_depressed, mods_latched,
                        mods_locked, 0, 0, group);
}

const struct wl_keyboard_listener Display::keyboard_listener = {
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .key = keyboard_handle_key,
    .modifiers = keyboard_handle_modifiers,
};

[[maybe_unused]] struct Display::touch_point* Display::get_touch_point(
    Display* d,
    [[maybe_unused]] int32_t id) {
  [[maybe_unused]] auto touch = &d->m_touch.event;

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
                                [[maybe_unused]] wl_fixed_t x_w,
                                [[maybe_unused]] wl_fixed_t y_w) {
  auto* d = static_cast<Display*>(data);

  [[maybe_unused]] bool first_down = (d->m_touch.down_count[id] == 0);
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

  if (d->m_flutter_engine) {
    [[maybe_unused]] bool last_up = (d->m_touch.down_count[id] == 0);
    d->m_flutter_engine->SendTouchEvent(
        (last_up ? FlutterPointerPhase::kUp : FlutterPointerPhase::kMove),
        wl_fixed_to_double(d->m_touch.surface_x),
        wl_fixed_to_double(d->m_touch.surface_y), id);
  }
}

void Display::touch_handle_motion(void* data,
                                  [[maybe_unused]] struct wl_touch* wl_touch,
                                  [[maybe_unused]] uint32_t time,
                                  [[maybe_unused]] int32_t id,
                                  [[maybe_unused]] wl_fixed_t x_w,
                                  [[maybe_unused]] wl_fixed_t y_w) {
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
  //  FML_DLOG(INFO) << "touch_handle_frame";
}

const struct wl_touch_listener Display::touch_listener = {
    .down = touch_handle_down,
    .up = touch_handle_up,
    .motion = touch_handle_motion,
    .frame = touch_handle_frame,
    .cancel = touch_handle_cancel,
};

[[maybe_unused]] void Display::AglShellDoBackground(
    struct wl_surface* surface) {
  if (m_agl_shell) {
    agl_shell_set_background(m_agl_shell, surface, m_output);
  }
}

[[maybe_unused]] void Display::AglShellDoPanel([[maybe_unused]] struct wl_surface* surface,
                                               [[maybe_unused]] enum agl_shell_edge mode) {
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

bool Display::ActivateSystemCursor([[maybe_unused]] int32_t device,
                                   const std::string& kind) {
  if (!m_enable_cursor) {
    FML_DLOG(INFO) << "[cursor_disabled]";
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

    [[maybe_unused]] auto cursor =
        wl_cursor_theme_get_cursor(m_cursor_theme, cursor_name);
    if (cursor == nullptr) {
      FML_DLOG(INFO) << "Cursor [" << cursor_name << "] not found";
      return false;
    }
    [[maybe_unused]] auto cursor_buffer =
        wl_cursor_image_get_buffer(cursor->images[0]);
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

void Display::SetTextInput(std::shared_ptr<TextInput> text_input) {
  m_text_input = std::move(text_input);
}

void Display::dump_output(output_info_t* output) {
  FML_DLOG(INFO) << "wl_output: " << output->output;
  FML_DLOG(INFO) << "global_id: " << output->global_id;
  FML_DLOG(INFO) << "width: " << output->width;
  FML_DLOG(INFO) << "height: " << output->height;
  FML_DLOG(INFO) << "physical_width: " << output->physical_width << " mm";
  FML_DLOG(INFO) << "physical_height: " << output->physical_height << " mm";
  FML_DLOG(INFO) << "refresh_rate: "
                 << (output->refresh_rate > 1000
                         ? output->refresh_rate / 1000.0
                         : (double)output->refresh_rate);
  FML_DLOG(INFO) << "scale: " << output->scale;
  FML_DLOG(INFO) << "done: " << (output->done ? "true" : "false");
}

void Display::handle_primary_surface_enter(void* data,
                                           struct wl_surface* wl_surface,
                                           struct wl_output* output) {
  (void)output;
  (void)wl_surface;
  auto* d = static_cast<Display*>(data);

  for (auto& out : d->m_all_outputs) {
    if (out->output == output) {
      dump_output(out.get());

      FML_DLOG(INFO) << "Entering output #" << out->global_id << ", scale "
                     << out->scale;
      d->m_current_output = out.get();
      d->m_last_buffer_scale = d->m_buffer_scale;
      d->m_buffer_scale = out->scale;
      break;
    }
  }
}

const struct wl_surface_listener Display::primary_surface_listener = {
    handle_primary_surface_enter};

void Display::SetSurface(wl_surface* surface) {
  m_surface = surface;
  // TODO: wl_surface_set_buffer_scale requires wl_compositor version >= 3
  // wl_surface_set_buffer_scale(m_surface, m_buffer_scale);
  wl_surface_add_listener(m_surface, &primary_surface_listener, this);
}
