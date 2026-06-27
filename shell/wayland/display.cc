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
#include "logging/logging.h"

#include <linux/input-event-codes.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include "config/common.h"

#include "asio/post.hpp"
#include "engine.h"
#include "main_loop_waker.h"
#include "timer.h"
#include "window.h"

extern void KeyCallback(FlutterDesktopViewControllerState* view_state,
                        bool released,
                        xkb_keysym_t keysym,
                        uint32_t xkb_scancode,
                        uint32_t modifiers);

extern void FocusLostCallback(FlutterDesktopViewControllerState* view_state);

extern void KeymapChangedCallback(FlutterDesktopViewControllerState* view_state,
                                  xkb_keymap* keymap);

Display::Display(const bool enable_cursor,
                 const std::string& ignore_wayland_event,
                 std::string cursor_theme_name,
                 const std::vector<Configuration::Config>& configs)
    : stop_events_flag_(false),
      event_thread_active_(false),
      m_enable_cursor(enable_cursor),
      m_cursor_theme_name(std::move(cursor_theme_name)),
      m_xkb_context(xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {
  SPDLOG_TRACE("+ Display()");

  wayland_event_mask_update(ignore_wayland_event, m_wayland_event_mask);

  // Build the compositor-protocol shells (xdg / agl / ivi / simple) in priority
  // order BEFORE registry enumeration so registry_handle_global can offer each
  // unclaimed global to them. The active shell is the first to bind (see
  // ActiveShell()). The selection ("auto" | "xdg" | "agl" | "ivi" | "simple")
  // comes from the first view's --shell / view.shell config; the Wayland
  // connection is shared, so one selection drives all views.
  const std::string shell_sel =
      configs.empty() ? std::string("auto")
                      : configs.front().view.shell.value_or("auto");
  shells_ = ivi::WaylandShell::Create(shell_sel);

  m_display = wl_display_connect(nullptr);
  if (m_display == nullptr) {
    spdlog::critical("Failed to connect to Wayland display. {}",
                     strerror(errno));
    exit(-1);
  }

  m_registry = wl_display_get_registry(m_display);
  wl_registry_add_listener(m_registry, &registry_listener, this);
  // First dispatch picks up registry globals (synchronous: the server emits
  // them immediately after we bind the registry). The roundtrip that follows
  // pulls in events the server emits ONLY in response to our binds — notably
  // wp_presentation.clock_id (now handled by the vsync provider).
  wl_display_dispatch(m_display);
  wl_display_roundtrip(m_display);

  // AGL's window-management facet maps output indices to wl_outputs owned here.
  for (auto& s : shells_) {
    s->SetOutputResolver([this](std::size_t i) -> wl_output* {
      return i < m_all_outputs.size() ? m_all_outputs[i]->output : nullptr;
    });
  }

  // Post-registry handshake (AGL roundtrips for agl_shell.bound_ok/bound_fail;
  // others are no-ops).
  for (auto& s : shells_) {
    if (!s->Sync(m_display)) {
      spdlog::critical("[Display] shell '{}' handshake failed", s->Name());
      exit(EXIT_FAILURE);
    }
  }

  SPDLOG_TRACE("- Display()");
}

ivi::WaylandShell& Display::ActiveShell() const {
  for (const auto& s : shells_) {
    if (s->IsBound()) {
      return *s;
    }
  }
  spdlog::critical("[Display] no compositor shell bound (no xdg/agl/ivi/simple)");
  exit(EXIT_FAILURE);
}

Display::~Display() {
  SPDLOG_TRACE("+ ~Display()");

  // The shells (xdg/agl/ivi/simple) and the vsync provider own their protocol
  // objects and tear them down in their own destructors.
  shells_.clear();

  if (m_shm)
    wl_shm_destroy(m_shm);

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

  SPDLOG_TRACE("- ~Display()");
}


void Display::registry_handle_global(void* data,
                                     struct wl_registry* registry,
                                     uint32_t name,
                                     const char* interface,
                                     uint32_t version) {
  auto* d = static_cast<Display*>(data);

  SPDLOG_DEBUG("Wayland: {} version {}", interface, version);

  // Note when GPU-buffer-allocation protocols are advertised. Mesa's
  // Vulkan WSI Wayland implementation needs one of these to back the
  // swapchain; vkGetPhysicalDeviceSurfaceFormatsKHR returns
  // VK_ERROR_SURFACE_LOST_KHR when both are absent. The flags let the
  // backend pre-flight and emit a clear error rather than asserting.
  if (strcmp(interface, "zwp_linux_dmabuf_v1") == 0) {
    d->m_has_linux_dmabuf = true;
  } else if (strcmp(interface, "wl_drm") == 0) {
    d->m_has_wl_drm = true;
  }

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    if (version >= 3) {
      d->m_compositor = static_cast<wl_compositor*>(
          wl_registry_bind(registry, name, &wl_compositor_interface,
                           std::min(static_cast<uint32_t>(3), version)));
      SPDLOG_DEBUG("\tBuffer Scale Enabled");
      d->m_buffer_scale_enable = true;
      if (d->m_shm) {
        d->m_cursor_surface = wl_compositor_create_surface(d->m_compositor);
      }
    } else {
      d->m_compositor = static_cast<wl_compositor*>(
          wl_registry_bind(registry, name, &wl_compositor_interface,
                           std::min(static_cast<uint32_t>(2), version)));
    }
    d->m_base_surface = wl_compositor_create_surface(d->m_compositor);
  } else if (strcmp(interface, wl_subcompositor_interface.name) == 0) {
    d->m_subcompositor = static_cast<wl_subcompositor*>(
        wl_registry_bind(registry, name, &wl_subcompositor_interface,
                         std::min(static_cast<uint32_t>(1), version)));
  }
  else if (strcmp(interface, wl_shm_interface.name) == 0) {
    d->m_shm = static_cast<wl_shm*>(
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
    const auto oi = std::make_shared<output_info_t>();
    std::fill_n(oi.get(), 1, output_info_t{});
    oi->global_id = name;
    oi->display = d;
    // be compat with v2 as well
#if defined(WL_OUTPUT_NAME_SINCE_VERSION) && \
    defined(WL_OUTPUT_DESCRIPTION_SINCE_VERSION)
    if (version >= WL_OUTPUT_NAME_SINCE_VERSION &&
        version >= WL_OUTPUT_DESCRIPTION_SINCE_VERSION)
      oi->output = static_cast<wl_output*>(
          wl_registry_bind(registry, name, &wl_output_interface,
                           std::min(static_cast<uint32_t>(4), version)));
    else
#endif
      oi->output = static_cast<wl_output*>(
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
  // wp_presentation is owned by the vsync provider.
  else if (d->m_vsync.TryBindGlobal(registry, name, interface, version)) {
  }
  // Everything else: offer the global to each compositor-protocol shell
  // (xdg/agl/ivi/simple); the first that claims it wins.
  else {
    for (auto& s : d->shells_) {
      if (s->TryBindGlobal(registry, name, interface, version)) {
        break;
      }
    }
  }
}

void Display::registry_handle_global_remove(void* /* data */,
                                            struct wl_registry* /* reg */,
                                            uint32_t /* id */) {}

const wl_registry_listener Display::registry_listener = {
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
                                      int transform) {
  auto* oi = static_cast<output_info_t*>(data);
  oi->physical_width = static_cast<unsigned int>(physical_width);
  oi->physical_height = static_cast<unsigned int>(physical_height);
  oi->transform = transform;

  SPDLOG_DEBUG("Physical width: {} mm x {} mm", physical_width,
               physical_height);
}

void Display::display_handle_mode(void* data,
                                  struct wl_output* /* wl_output */,
                                  uint32_t flags,
                                  int width,
                                  int height,
                                  int refresh) {
  auto* oi = static_cast<output_info_t*>(data);

  if ((flags & WL_OUTPUT_MODE_CURRENT) == WL_OUTPUT_MODE_CURRENT) {
    oi->height = static_cast<unsigned int>(height);
    oi->width = static_cast<unsigned int>(width);
    oi->refresh_rate = refresh / 1000.0;
    if (oi->display) {
      oi->display->NotifyOutputResized(oi);
    }
  }

  SPDLOG_DEBUG("Video mode: {} x {} @ {} Hz", width, height, refresh / 1000.0);
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

void Display::display_handle_name(void* data,
                                  struct wl_output* /* wl_output */,
                                  const char* name) {
  auto* oi = static_cast<output_info_t*>(data);
  oi->name = std::string(name);
}

void Display::display_handle_desc(void* data,
                                  struct wl_output* /* wl_output */,
                                  const char* desc) {
  auto* oi = static_cast<output_info_t*>(data);
  oi->desc = std::string(desc);
}

const wl_output_listener Display::output_listener = {display_handle_geometry,
                                                     display_handle_mode,
                                                     display_handle_done,
                                                     display_handle_scale
#if defined(WL_OUTPUT_NAME_SINCE_VERSION) && \
    defined(WL_OUTPUT_DESCRIPTION_SINCE_VERSION)
                                                     ,
                                                     display_handle_name,
                                                     display_handle_desc
#endif
};

void Display::shm_format(void* /* data */,
                         struct wl_shm* /* wl_shm */,
                         uint32_t /* format */) {}

const wl_shm_listener Display::shm_listener = {shm_format};

void Display::seat_handle_capabilities(void* data,
                                       struct wl_seat* seat,
                                       uint32_t caps) {
  auto* d = static_cast<Display*>(data);

  if (!d->m_wayland_event_mask.pointer) {
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !d->m_pointer.wl_pointer) {
      spdlog::debug("Pointer Present");
      d->m_pointer.wl_pointer = wl_seat_get_pointer(seat);
      wl_pointer_add_listener(d->m_pointer.wl_pointer, &pointer_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) &&
               d->m_pointer.wl_pointer) {
      wl_pointer_release(d->m_pointer.wl_pointer);
      d->m_pointer.wl_pointer = nullptr;
    }
  }

  if (!d->m_wayland_event_mask.keyboard) {
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !d->m_keyboard) {
      spdlog::debug("Keyboard Present");
      d->m_keyboard = wl_seat_get_keyboard(seat);
      wl_keyboard_add_listener(d->m_keyboard, &keyboard_listener, d);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->m_keyboard) {
      wl_keyboard_release(d->m_keyboard);
      d->m_keyboard = nullptr;
    }
  }

  if (!d->m_wayland_event_mask.touch) {
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->m_touch.touch) {
      spdlog::debug("Touch Present");
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

const wl_seat_listener Display::seat_listener = {
    .capabilities = seat_handle_capabilities,
    .name = seat_handle_name,
};

bool Display::pointerButtonStatePressed(struct pointer const* p) {
  return (p->buttons) && (p->event.state == WL_POINTER_BUTTON_STATE_PRESSED);
}

void Display::pointer_handle_enter(void* data,
                                   struct wl_pointer* /* pointer */,
                                   uint32_t serial,
                                   struct wl_surface* surface,
                                   wl_fixed_t sx,
                                   wl_fixed_t sy) {
  auto* d = static_cast<Display*>(data);
  d->m_active_surface = surface;
  d->m_active_engine = d->m_surface_engine_map[surface];

  d->m_pointer.event.surface_x = wl_fixed_to_double(sx);
  d->m_pointer.event.surface_y = wl_fixed_to_double(sy);
  d->m_pointer.serial = serial;

  if (d->m_active_engine) {
    d->m_active_engine->CoalesceMouseEvent(
        kFlutterPointerSignalKindNone, kAdd, d->m_pointer.event.surface_x,
        d->m_pointer.event.surface_y, 0.0, 0.0, d->m_pointer.buttons);
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
                                           kRemove, 0.0, 0.0, 0.0, 0.0,
                                           d->m_pointer.buttons);
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
      const FlutterPointerPhase phase =
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
  if (auto* d = static_cast<Display*>(data);
      !d->m_wayland_event_mask.pointer_axis) {
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

const wl_pointer_listener Display::pointer_listener = {
    .enter = pointer_handle_enter,
    .leave = pointer_handle_leave,
    .motion = pointer_handle_motion,
    .button = pointer_handle_button,
    .axis = pointer_handle_axis,
    .frame = pointer_handle_frame,
    .axis_source = pointer_handle_axis_source,
    .axis_stop = pointer_handle_axis_stop,
    .axis_discrete = pointer_handle_axis_discrete,
#if defined(WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
    .axis_value120 = nullptr,
#endif
#if defined(WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
    .axis_relative_direction = nullptr,
#endif
};

void Display::keyboard_handle_enter(void* data,
                                    struct wl_keyboard* /* keyboard */,
                                    uint32_t /* serial */,
                                    struct wl_surface* surface,
                                    struct wl_array* /* keys */) {
  SPDLOG_TRACE("+ Display::keyboard_handle_enter()");
  auto* d = static_cast<Display*>(data);
  d->m_active_surface = surface;
  d->m_active_engine = d->m_surface_engine_map[surface];
  SPDLOG_TRACE("- Display::keyboard_handle_enter()");
}

void Display::keyboard_handle_leave(void* data,
                                    struct wl_keyboard* /* keyboard */,
                                    uint32_t /* serial */,
                                    struct wl_surface* /* surface */) {
  SPDLOG_TRACE("+ Display::keyboard_handle_leave()");
  auto* d = static_cast<Display*>(data);

  d->m_repeat_timer->disarm();
  set_repeat_code(d, XKB_KEY_NoSymbol);
  SPDLOG_TRACE("- Display::keyboard_handle_leave()");
  // Post FocusLostCallback onto the platform strand so that all
  // TextInputPlugin state mutations are serialised with key-event callbacks.
  if (d->m_view_controller_state) {
    auto* vcs = d->m_view_controller_state;
    if (vcs->engine && vcs->engine->GetPlatformTaskRunner()) {
      asio::post(*vcs->engine->GetPlatformTaskRunner()->GetStrandContext(),
                 [vcs]() { FocusLostCallback(vcs); });
    }
  }
}

void Display::keyboard_handle_keymap(void* data,
                                     struct wl_keyboard* /* keyboard */,
                                     uint32_t /* format */,
                                     int fd,
                                     uint32_t size) {
  auto* d = static_cast<Display*>(data);
  const auto keymap_string =
      static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0));
  xkb_keymap_unref(d->m_keymap);
  d->m_keymap = xkb_keymap_new_from_string(d->m_xkb_context, keymap_string,
                                           XKB_KEYMAP_FORMAT_TEXT_V1,
                                           XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(keymap_string, size);
  close(fd);
  // Disarm the repeat timer before swapping xkb_state so that the repeat
  // callback cannot read the old (about-to-be-freed) xkb_state.
  d->m_repeat_timer->disarm();
  set_repeat_code(d, XKB_KEY_NoSymbol);
  xkb_state_unref(d->m_xkb_state);
  d->m_xkb_state = xkb_state_new(d->m_keymap);
  // Post KeymapChangedCallback onto the platform strand.
  // Refcount the keymap so it stays alive until the lambda fires.
  if (d->m_view_controller_state && d->m_keymap) {
    auto* vcs = d->m_view_controller_state;
    if (vcs->engine && vcs->engine->GetPlatformTaskRunner()) {
      xkb_keymap_ref(d->m_keymap);
      auto* km = d->m_keymap;
      asio::post(*vcs->engine->GetPlatformTaskRunner()->GetStrandContext(),
                 [vcs, km]() {
                   KeymapChangedCallback(vcs, km);
                   xkb_keymap_unref(km);
                 });
    }
  }
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
  const uint32_t modifiers = d->m_mods_effective;

  if (keysym == XKB_KEY_NoSymbol) {
    const xkb_keysym_t* key_symbols;
    const int res =
        xkb_state_key_get_syms(d->m_xkb_state, xkb_scancode, &key_symbols);
    if (res == 0) {
      spdlog::debug("xkb_scancode has no key symbols: 0x{:x}", xkb_scancode);
      keysym = XKB_KEY_NoSymbol;
    } else {
      // only use the first symbol until the use case for two is clarified
      keysym = key_symbols[0];
      for (int i = 0; i < res; i++) {
        spdlog::debug("xkb keysym: 0x{:x}", key_symbols[i]);
      }
    }
  }

  if (d->m_view_controller_state) {
    KeyCallback(d->m_view_controller_state,
                state == WL_KEYBOARD_KEY_STATE_RELEASED, keysym, xkb_scancode,
                modifiers);
  }

  if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    if (xkb_keymap_key_repeats(d->m_keymap, xkb_scancode)) {
      SPDLOG_DEBUG("xkb_keymap_key_repeats: 0x{:x}", xkb_scancode);
      d->m_keysym_pressed = keysym;
      set_repeat_code(d, xkb_scancode);
      d->m_repeat_timer->arm();
      // Arming happens on the Wayland event thread; the main loop may be
      // blocked idle (App::Loop now blocks when HasRepeatTimer() is false).
      // Wake it so it re-evaluates and starts pacing the key-repeat promptly
      // instead of after the idle heartbeat.
      MainLoopWaker::instance().Wake();
    } else {
      SPDLOG_DEBUG("key does not repeat: 0x{:x}", xkb_scancode);
    }

  } else if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
    if (d->m_repeat_code == xkb_scancode) {
      d->m_repeat_timer->disarm();
      set_repeat_code(d, XKB_KEY_NoSymbol);
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
  // Cache the effective modifier mask so key and repeat callbacks can
  // read it without calling xkb_state_serialize_mods themselves.
  d->m_mods_effective =
      xkb_state_serialize_mods(d->m_xkb_state, XKB_STATE_MODS_EFFECTIVE);
}

void Display::keyboard_handle_repeat_info(void* data,
                                          struct wl_keyboard* /* wl_keyboard */,
                                          int32_t rate,
                                          int32_t delay) {
  const auto d = static_cast<Display*>(data);
  d->m_repeat_timer->set_timerspec(rate, delay);
  SPDLOG_DEBUG("[keyboard repeat info] rate: {}, delay: {}", rate, delay);
}

const wl_keyboard_listener Display::keyboard_listener = {
    .keymap = keyboard_handle_keymap,
    .enter = keyboard_handle_enter,
    .leave = keyboard_handle_leave,
    .key = keyboard_handle_key,
    .modifiers = keyboard_handle_modifiers,
    .repeat_info = keyboard_handle_repeat_info,
};

void Display::keyboard_repeat_func(void* data) {
  if (auto d = static_cast<Display*>(data);
      XKB_KEY_NoSymbol != d->m_repeat_code) {
    if (d->m_view_controller_state) {
      KeyCallback(d->m_view_controller_state, false, d->m_keysym_pressed,
                  d->m_repeat_code, d->m_mods_effective);
    }
  }
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
  const auto* d = static_cast<Display*>(data);

  if (d->m_touch_engine) {
    d->m_touch_engine->CoalesceTouchEvent(
        kUp, wl_fixed_to_double(d->m_touch.surface_x[id]),
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
  const auto* d = static_cast<Display*>(data);
  if (d->m_touch_engine) {
    SPDLOG_DEBUG("touch_handle_cancel");
    d->m_touch_engine->CoalesceTouchEvent(kCancel, d->m_pointer.event.surface_x,
                                          d->m_pointer.event.surface_y, 0);
  }
}

void Display::touch_handle_frame(void* /* data */,
                                 struct wl_touch* /* wl_touch */) {}

constexpr wl_touch_listener Display::touch_listener = {
    .down = touch_handle_down,
    .up = touch_handle_up,
    .motion = touch_handle_motion,
    .frame = touch_handle_frame,
    .cancel = touch_handle_cancel,
#if defined(WL_TOUCH_SHAPE_SINCE_VERSION)
    .shape = nullptr,
#endif
#if defined(WL_TOUCH_ORIENTATION_SINCE_VERSION)
    .orientation = nullptr,
#endif
};

int Display::PollEvents() const {
  while (wl_display_prepare_read(m_display) != 0) {
    wl_display_dispatch_pending(m_display);
  }
  wl_display_flush(m_display);

  wl_display_read_events(m_display);
  return wl_display_dispatch_pending(m_display);
}

void Display::StartEvents() {
  if (event_thread_active_)
    return;

  event_thread_ = std::thread([this] {
    event_thread_active_ = true;
    while (!stop_events_flag_) {
      const int count = wl_display_dispatch(m_display);
      if (count == -1) {
        spdlog::error("Wayland Dispatch Error: {}", strerror(errno));
        break;
      }
      SPDLOG_TRACE("Wayland Event Count: {}", count);
    }
    event_thread_active_ = false;
  });
}

void Display::StopEvents() {
  if (!event_thread_active_)
    return;

  stop_events_flag_ = true;
  if (event_thread_.joinable()) {
    event_thread_.join();
  }
}


void Display::SetEngine(wl_surface* surface, Engine* engine) {
  m_active_engine = engine;
  m_active_surface = surface;
  m_surface_engine_map[surface] = engine;
}

void Display::RegisterWindow(WaylandWindow* window) {
  if (!window) {
    return;
  }
  std::lock_guard lock(m_windows_lock);
  m_windows.push_back(window);
}

void Display::UnregisterWindow(WaylandWindow* window) {
  std::lock_guard lock(m_windows_lock);
  m_windows.erase(std::remove(m_windows.begin(), m_windows.end(), window),
                  m_windows.end());
}

size_t Display::IndexOfOutput(const output_info_t* oi) const {
  for (size_t i = 0; i < m_all_outputs.size(); ++i) {
    if (m_all_outputs[i].get() == oi) {
      return i;
    }
  }
  return m_all_outputs.size();
}

void Display::NotifyOutputResized(const output_info_t* oi) {
  const size_t idx = IndexOfOutput(oi);
  if (idx >= m_all_outputs.size()) {
    return;
  }
  const auto width = static_cast<int32_t>(oi->width);
  const auto height = static_cast<int32_t>(oi->height);
  std::vector<WaylandWindow*> snapshot;
  {
    std::lock_guard lock(m_windows_lock);
    snapshot = m_windows;
  }
  for (auto* w : snapshot) {
    w->OnOutputResized(idx, width, height);
  }
}

bool Display::ActivateSystemCursor(const int32_t device,
                                   const std::string& kind) const {
  (void)device;
  if (!m_enable_cursor) {
    wl_pointer_set_cursor(m_pointer.wl_pointer, m_pointer.serial, nullptr, 0,
                          0);
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

    const auto cursor = wl_cursor_theme_get_cursor(m_cursor_theme, cursor_name);
    if (cursor == nullptr) {
      SPDLOG_DEBUG("Cursor [{}] not found", cursor_name);
      return false;
    }
    const auto cursor_buffer = wl_cursor_image_get_buffer(cursor->images[0]);
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

int32_t Display::GetBufferScale(uint32_t index) const {
  if (index < m_all_outputs.size()) {
    if (m_buffer_scale_enable) {
      if (m_all_outputs[index]->scale == 0) {
        return 1;
      }
      return m_all_outputs[index]->scale;
    }
    return static_cast<int32_t>(kDefaultBufferScale);
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

double Display::GetRefreshRate(uint32_t index) const {
  if (index < m_all_outputs.size()) {
    return m_all_outputs[index]->refresh_rate;
  }
  SPDLOG_DEBUG("GetRefreshRate: Invalid output index: {}", index);
  return 0;
}

double Display::GetMaxRefreshRate() const {
  double max_refresh_rate = 0;
  for (const auto& output : m_all_outputs) {
    max_refresh_rate = std::max(max_refresh_rate, output->refresh_rate);
  }
  return max_refresh_rate;
}



void Display::wayland_event_mask_print(struct wayland_event_mask const& mask) {
  const std::string out;
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

  std::transform(
      events.begin(), events.end(), events.begin(),
      [](const char c) { return std::tolower(static_cast<unsigned char>(c)); });

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
