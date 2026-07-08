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
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <system_error>
#include <utility>

#include "config/common.h"

#include "fractional_scale_v1_client.hpp"
#include "viewporter_client.hpp"

#include "asio/post.hpp"
#include "engine.h"
#include "window.h"

extern void KeyCallback(FlutterDesktopViewControllerState* view_state,
                        bool released,
                        xkb_keysym_t keysym,
                        uint32_t xkb_scancode,
                        uint32_t modifiers);

extern void KeymapChangedCallback(FlutterDesktopViewControllerState* view_state,
                                  xkb_keymap* keymap);

extern void FocusLostCallback(FlutterDesktopViewControllerState* view_state);

Display::Display(const bool enable_cursor,
                 const std::string& ignore_wayland_event,
                 std::string cursor_theme_name,
                 const std::vector<Configuration::Config>& configs)
    : m_enable_cursor(enable_cursor),
      m_cursor_theme_name(std::move(cursor_theme_name)) {
  SPDLOG_TRACE("+ Display()");

  wayland_event_mask_update(ignore_wayland_event, m_wayland_event_mask);

  // Motion-to-photon profiling: feed the vsync provider's presented event when
  // enabled. The input side is fed from the pointer/touch handlers (NoteInput).
  m_m2p_enabled = profiling::MotionToPhoton::Enabled();
  if (m_m2p_enabled) {
    m_vsync.SetMotionToPhoton(&m_m2p);
  }

  // Build the compositor-protocol shells (xdg / agl / ivi / simple) in priority
  // order BEFORE registry enumeration so HandleGlobal can offer each
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

  // Own the registry as a wl::CRegistry so the text-input backend (which binds
  // through it) can share it; the per-global binds below stay raw via
  // reg.Get().
  if (!registry_.Create(m_display)) {
    spdlog::critical("Failed to create Wayland registry.");
    exit(-1);
  }
  registry_.OnGlobal(
      [this](wl::CRegistry& reg, uint32_t name, std::string_view iface,
             uint32_t ver) { HandleGlobal(reg, name, iface, ver); });
  registry_.OnRemove([this](wl::CRegistry& /* reg */, const uint32_t name) {
    HandleGlobalRemove(name);
  });
  // First dispatch picks up registry globals (synchronous: the server emits
  // them immediately after we bind the registry). The roundtrip that follows
  // pulls in events the server emits ONLY in response to our binds — notably
  // wp_presentation.clock_id (now handled by the vsync provider).
  wl_display_dispatch(m_display);
  wl_display_roundtrip(m_display);

  // Adopt the text-input backend now that the seat (if any) is bound. Bind is a
  // graceful no-op returning true when zwp_text_input_manager_v3 was never
  // advertised; it creates the per-seat text_input from m_seat otherwise.
  if (m_seat &&
      !text_input_.Bind(registry_, this, reinterpret_cast<wl_proxy*>(m_seat))) {
    spdlog::warn("[Display] text-input manager advertised but bind failed");
  }

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

void Display::NoteInput(const uint64_t ts_us) {
  if (!m_m2p_enabled) {
    return;
  }
  uint64_t ns = ts_us * 1000ULL;
  if (ts_us == 0) {
    // No high-resolution source: fall back to arrival time (same clock domain).
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    ns = static_cast<uint64_t>(t.tv_sec) * 1'000'000'000ULL +
         static_cast<uint64_t>(t.tv_nsec);
  }
  m_m2p.RecordInput(ns);
}

ivi::WaylandShell& Display::ActiveShell() const {
  for (const auto& s : shells_) {
    if (s->IsBound()) {
      return *s;
    }
  }
  spdlog::critical(
      "[Display] no compositor shell bound (no xdg/agl/ivi/simple)");
  exit(EXIT_FAILURE);
}

Display::~Display() {
  SPDLOG_TRACE("+ ~Display()");

  if (m_m2p_enabled) {
    m_m2p.LogSessionSummary("wayland");
    m_vsync.SetMotionToPhoton(nullptr);  // detach before m_m2p is destroyed
  }

  // Detach the async descriptors from the fds they were watching. App owns and
  // has already stopped the shared reactor; both fds are owned elsewhere (the
  // repeat timerfd by the keyboard handler, the display fd by libwayland), so
  // release() — never close() — them before the optionals are destroyed.
  ReleaseRepeatFd();
  ReleaseWaylandFd();
  // Tear down the text_input/manager proxies before the seat + display go away.
  text_input_.Release();
  // Send wl_keyboard teardown + free the handler's xkb objects/timerfd.
  keyboard_.Reset();
  // Destroy the zwp_input_timestamps_v1 subscriptions + manager while the
  // display connection is still alive.
  input_timestamps_.Reset();

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

  // Destroy the registry before wl_display_disconnect: wl_registry_destroy
  // touches the display mutex inside libwayland.
  registry_.Reset();
  wl_display_flush(m_display);
  wl_display_disconnect(m_display);

  SPDLOG_TRACE("- ~Display()");
}

void Display::HandleGlobal(wl::CRegistry& reg,
                           uint32_t name,
                           std::string_view iface,
                           uint32_t version) {
  auto* d = this;
  // The raw wl_registry* the per-global binds + the vsync/shells chain bind
  // through (they take the C handle; CRegistry owns it).
  wl_registry* registry = reg.Get();
  // C-string view for the strcmp-based comparisons retained below.
  const char* interface = iface.data();

  SPDLOG_DEBUG("Wayland: {} version {}", iface, version);

  // Note when GPU-buffer-allocation protocols are advertised. Mesa's
  // Vulkan WSI Wayland implementation needs one of these to back the
  // swapchain; vkGetPhysicalDeviceSurfaceFormatsKHR returns
  // VK_ERROR_SURFACE_LOST_KHR when both are absent. The flags let the
  // backend pre-flight and emit a clear error rather than asserting.
  if (iface == "zwp_linux_dmabuf_v1") {
    d->m_has_linux_dmabuf = true;
  } else if (iface == "wl_drm") {
    d->m_has_wl_drm = true;
  }

  if (iface == wl_compositor_interface.name) {
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
  } else if (iface == wl_subcompositor_interface.name) {
    d->m_subcompositor = static_cast<wl_subcompositor*>(
        wl_registry_bind(registry, name, &wl_subcompositor_interface,
                         std::min(static_cast<uint32_t>(1), version)));
  } else if (iface == wl_shm_interface.name) {
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
  } else if (iface == wl_output_interface.name) {
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
  } else if (iface == wl_seat_interface.name) {
    d->m_seat = static_cast<wl_seat*>(
        wl_registry_bind(registry, name, &wl_seat_interface,
                         std::min(static_cast<uint32_t>(5), version)));
    wl_seat_add_listener(d->m_seat, &seat_listener, d);
  } else if (iface ==
             viewporter::client::wp_viewporter_traits::interface_name) {
    d->m_viewporter = static_cast<wl_proxy*>(wl_registry_bind(
        registry, name, &viewporter::client::wp_viewporter_traits::wl_iface(),
        std::min(viewporter::client::wp_viewporter_traits::version, version)));
  } else if (iface ==
             fractional_scale_v1::client::
                 wp_fractional_scale_manager_v1_traits::interface_name) {
    d->m_fractional_scale_manager = static_cast<wl_proxy*>(wl_registry_bind(
        registry, name,
        &fractional_scale_v1::client::wp_fractional_scale_manager_v1_traits::
            wl_iface(),
        std::min(fractional_scale_v1::client::
                     wp_fractional_scale_manager_v1_traits::version,
                 version)));
  }
  // zwp_text_input_manager_v3 — recorded for text_input_.Bind() after the
  // startup roundtrip (the per-seat text_input needs m_seat). If the compositor
  // never advertises it, text_input_ stays unbound and all IME paths no-op.
  else if (iface == "zwp_text_input_manager_v3") {
    d->text_input_.Record(name, version);
  }
  // wp_presentation is owned by the vsync provider;
  // zwp_input_timestamps_manager_v1 by the input-timestamps provider (which
  // subscribes any devices the seat has already announced). Short-circuit ||:
  // each global is offered to at most one owner.
  else if (d->m_vsync.TryBindGlobal(registry, name, interface, version) ||
           d->input_timestamps_.TryBindGlobal(registry, name, interface,
                                              version)) {
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
  oi->display->NotifyOutputScaleChanged(oi);
  oi->display->EmitOutputDone(oi);
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
      d->input_timestamps_.AttachPointer(d->m_pointer.wl_pointer);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) &&
               d->m_pointer.wl_pointer) {
      // Detach (destroys the subscription proxy) before releasing the device
      // it references.
      d->input_timestamps_.AttachPointer(nullptr);
      wl_pointer_release(d->m_pointer.wl_pointer);
      d->m_pointer.wl_pointer = nullptr;
    }
  }

  if (!d->m_wayland_event_mask.keyboard) {
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && d->keyboard_.IsNull()) {
      spdlog::debug("Keyboard Present");
      // Wrap the raw wl_keyboard in the scanner's CRTP handler (installs its
      // listener); set the back-pointer and register its repeat timerfd on the
      // reactor. This runs on the reactor thread during wl dispatch, so OnKey,
      // the keyboard events and DispatchRepeat all share one thread.
      wl_keyboard* raw = wl_seat_get_keyboard(seat);
      if (wl::SetupHandler(d->keyboard_, reinterpret_cast<wl_proxy*>(raw))) {
        d->keyboard_.Get()->app_ = d;
        d->ArmRepeatFd();
      }
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) &&
               !d->keyboard_.IsNull()) {
      d->ReleaseRepeatFd();
      d->keyboard_.Reset();
    }
  }

  if (!d->m_wayland_event_mask.touch) {
    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !d->m_touch.touch) {
      spdlog::debug("Touch Present");
      d->m_touch.touch = wl_seat_get_touch(seat);
      wl_touch_set_user_data(d->m_touch.touch, d);
      wl_touch_add_listener(d->m_touch.touch, &touch_listener, d);
      d->input_timestamps_.AttachTouch(d->m_touch.touch);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && d->m_touch.touch) {
      // Detach (destroys the subscription proxy) before releasing the device
      // it references.
      d->input_timestamps_.AttachTouch(nullptr);
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
  // Take BEFORE the mask gate: motion carries a wl timestamp, so a
  // zwp_input_timestamps_v1 stamp may be pending for it. Consuming it even
  // when the event is dropped keeps a stale stamp from attaching to a later
  // event.
  const uint64_t ts_us = d->input_timestamps_.TakePointerTimeUs();
  d->NoteInput(ts_us);

  if (!d->m_wayland_event_mask.pointer_motion) {
    d->m_pointer.event.surface_x = wl_fixed_to_double(sx);
    d->m_pointer.event.surface_y = wl_fixed_to_double(sy);

    if (d->m_active_engine) {
      const FlutterPointerPhase phase =
          pointerButtonStatePressed(&d->m_pointer) ? kMove : kHover;
      d->m_active_engine->CoalesceMouseEvent(
          kFlutterPointerSignalKindNone, phase, d->m_pointer.event.surface_x,
          d->m_pointer.event.surface_y, 0.0, 0.0, d->m_pointer.buttons, ts_us);
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
  // Take before the mask gate (see pointer_handle_motion).
  const uint64_t ts_us = d->input_timestamps_.TakePointerTimeUs();
  d->NoteInput(ts_us);
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
          d->m_pointer.event.surface_y, 0.0, 0.0, d->m_pointer.buttons, ts_us);
    }
  }
}

void Display::pointer_handle_axis(void* data,
                                  struct wl_pointer* /* wl_pointer */,
                                  uint32_t time,
                                  uint32_t axis,
                                  wl_fixed_t value) {
  auto* d = static_cast<Display*>(data);
  // Take before the mask gate (see pointer_handle_motion).
  const uint64_t ts_us = d->input_timestamps_.TakePointerTimeUs();
  d->NoteInput(ts_us);
  if (!d->m_wayland_event_mask.pointer_axis) {
    d->m_pointer.event.time = time;
    d->m_pointer.event.axes[axis].value = wl_fixed_to_double(value);

    if (d->m_active_engine) {
      d->m_active_engine->CoalesceMouseEvent(
          kFlutterPointerSignalKindScroll, FlutterPointerPhase::kMove,
          d->m_pointer.event.surface_x, d->m_pointer.event.surface_y,
          d->m_pointer.event.axes[1].value, d->m_pointer.event.axes[0].value,
          d->m_pointer.buttons, ts_us);
    }
  }
}

void Display::pointer_handle_frame(void* /* data */,
                                   struct wl_pointer* /* wl_pointer */) {}

void Display::pointer_handle_axis_source(void* /* data */,
                                         struct wl_pointer* /* wl_pointer */,
                                         uint32_t /* axis_source */) {}

void Display::pointer_handle_axis_stop(void* data,
                                       struct wl_pointer* /* wl_pointer */,
                                       uint32_t /* time */,
                                       uint32_t /* axis */) {
  // axis_stop carries a wl timestamp, so a high-resolution stamp may be
  // pending for it even though the event itself is not forwarded; discard it
  // so it can't attach to the next motion/button/axis.
  (void)static_cast<Display*>(data)->input_timestamps_.TakePointerTimeUs();
}

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

void Display::OnKey(const wl::KeyEvent& ev) {
  // Runs on the reactor thread for both real key events and repeat ticks. The
  // handler already resolved keysym + effective modifiers from its xkb_state;
  // mirror the old keyboard_handle_key dispatch exactly (evdev → XKB scancode
  // is ev.key + 8). KeyCallback re-posts onto the engine strand.
  if (!m_view_controller_state) {
    return;
  }
  KeyCallback(m_view_controller_state,
              ev.state == WL_KEYBOARD_KEY_STATE_RELEASED, ev.keysym,
              ev.key + 8u, ev.modifiers);
}

void Display::OnKeymap(xkb_keymap* keymap) {
  // The handler owns the compiled keymap/xkb_state; we only forward a ref to
  // the engine. Post KeymapChangedCallback onto the platform strand so the
  // TextInputPlugin state mutation is serialized with the key-event callbacks;
  // ref the keymap so it outlives the post.
  if (!m_view_controller_state || !keymap) {
    return;
  }
  auto* vcs = m_view_controller_state;
  if (vcs->engine && vcs->engine->GetPlatformTaskRunner()) {
    xkb_keymap_ref(keymap);
    asio::post(*vcs->engine->GetPlatformTaskRunner()->GetStrandContext(),
               [vcs, keymap]() {
                 KeymapChangedCallback(vcs, keymap);
                 xkb_keymap_unref(keymap);
               });
  }
}

void Display::OnKeyboardLeave() {
  // Keyboard focus left the surface. Post FocusLostCallback onto the platform
  // strand so the TextInputPlugin preedit reset is serialized with the
  // key-event callbacks. (wl::KeyboardHandler already stopped key-repeat.)
  if (!m_view_controller_state) {
    return;
  }
  auto* vcs = m_view_controller_state;
  if (vcs->engine && vcs->engine->GetPlatformTaskRunner()) {
    asio::post(*vcs->engine->GetPlatformTaskRunner()->GetStrandContext(),
               [vcs]() { FocusLostCallback(vcs); });
  }
}

// ── wl::ime::TextInputListener — compositor IME events (reactor thread)
// ───────
//
// Each callback posts onto the engine platform strand and drives the
// TextInputPlugin, mirroring OnKeyboardLeave so IME edits serialize with the
// key-event path. The plugin is owned by the view-controller state and only
// touched on that strand.

void Display::OnEnter() {
  // The field gained IME focus. Activation intent is driven from the plugin
  // (TextInput.show), and the backend auto-enables on enter; nothing to do.
}

void Display::OnLeave() {
  // The field lost IME focus. Clear any in-progress preedit so a stale
  // composing run doesn't linger; post onto the engine strand.
  if (!m_view_controller_state) {
    return;
  }
  auto* vcs = m_view_controller_state;
  if (vcs->engine && vcs->engine->GetPlatformTaskRunner() &&
      vcs->text_input_plugin) {
    auto* plugin = vcs->text_input_plugin.get();
    asio::post(*vcs->engine->GetPlatformTaskRunner()->GetStrandContext(),
               [plugin]() { plugin->SetPreeditString(std::string{}, 0, 0); });
  }
}

void Display::OnCommitString(std::string_view utf8) {
  if (!m_view_controller_state) {
    return;
  }
  auto* vcs = m_view_controller_state;
  if (vcs->engine && vcs->engine->GetPlatformTaskRunner() &&
      vcs->text_input_plugin) {
    auto* plugin = vcs->text_input_plugin.get();
    std::string text(utf8);
    asio::post(
        *vcs->engine->GetPlatformTaskRunner()->GetStrandContext(),
        [plugin, text = std::move(text)]() { plugin->CommitString(text); });
  }
}

void Display::OnPreeditString(std::string_view utf8,
                              int32_t cursor_begin,
                              int32_t cursor_end) {
  if (!m_view_controller_state) {
    return;
  }
  auto* vcs = m_view_controller_state;
  if (vcs->engine && vcs->engine->GetPlatformTaskRunner() &&
      vcs->text_input_plugin) {
    auto* plugin = vcs->text_input_plugin.get();
    std::string text(utf8);
    asio::post(*vcs->engine->GetPlatformTaskRunner()->GetStrandContext(),
               [plugin, text = std::move(text), cursor_begin, cursor_end]() {
                 plugin->SetPreeditString(text, cursor_begin, cursor_end);
               });
  }
}

void Display::OnDeleteSurroundingText(uint32_t before_bytes,
                                      uint32_t after_bytes) {
  if (!m_view_controller_state) {
    return;
  }
  auto* vcs = m_view_controller_state;
  if (vcs->engine && vcs->engine->GetPlatformTaskRunner() &&
      vcs->text_input_plugin) {
    auto* plugin = vcs->text_input_plugin.get();
    asio::post(*vcs->engine->GetPlatformTaskRunner()->GetStrandContext(),
               [plugin, before_bytes, after_bytes]() {
                 plugin->DeleteSurrounding(static_cast<int>(before_bytes),
                                           static_cast<int>(after_bytes));
               });
  }
}

// ── Text-input drivers (engine strand -> reactor thread) ─────────────────────
//
// Posted onto the reactor io_context so the text_input_ proxy is only mutated
// on the reactor thread. All are graceful no-ops while text_input_ is unbound.

void Display::ActivateTextInput(uint32_t hint,
                                uint32_t purpose,
                                int32_t x,
                                int32_t y,
                                int32_t width,
                                int32_t height) {
  if (!ioc_) {
    return;
  }
  asio::post(*ioc_, [this, hint, purpose, x, y, width, height]() {
    text_input_.SetContentType(static_cast<wl::ime::ContentHint>(hint),
                               static_cast<wl::ime::ContentPurpose>(purpose));
    text_input_.SetCursorRectangle(x, y, width, height);
    text_input_.Activate();
    text_input_.Commit();
  });
}

void Display::DeactivateTextInput() {
  if (!ioc_) {
    return;
  }
  asio::post(*ioc_, [this]() {
    text_input_.Deactivate();
    text_input_.Commit();
  });
}

void Display::UpdateTextInputCursorRect(int32_t x,
                                        int32_t y,
                                        int32_t width,
                                        int32_t height) {
  if (!ioc_) {
    return;
  }
  asio::post(*ioc_, [this, x, y, width, height]() {
    text_input_.SetCursorRectangle(x, y, width, height);
    text_input_.Commit();
  });
}

void Display::UpdateTextInputSurrounding(const std::string& utf8,
                                         uint32_t cursor,
                                         uint32_t anchor) {
  if (!ioc_) {
    return;
  }
  asio::post(*ioc_, [this, utf8, cursor, anchor]() {
    text_input_.SetSurroundingText(utf8, cursor, anchor);
    text_input_.Commit();
  });
}

// All wl_touch handlers run on the wayland event thread, so m_touch needs no
// locking. down/up/motion only accumulate into m_touch.frame; the batch is
// handed to the engine on wl_touch.frame — the protocol's atomicity boundary
// (everything between two frame events is one logically-simultaneous hardware
// scan). A 10-finger update therefore reaches the engine as one group (one
// lock, one main-loop wake, one FlutterEngineSendPointerEvent call → one
// UI-thread task in the engine) instead of ten independent submissions that
// can be split across flushes mid-scan.

void Display::touch_handle_down(void* data,
                                struct wl_touch* /* wl_touch */,
                                uint32_t /* serial */,
                                uint32_t /* time */,
                                struct wl_surface* surface,
                                int32_t id,
                                wl_fixed_t x_w,
                                wl_fixed_t y_w) {
  auto* d = static_cast<Display*>(data);
  // Take FIRST: the pending high-res stamp belongs to this event, not to the
  // frame a possible engine-switch flush below sends out.
  const uint64_t ts_us = d->input_timestamps_.TakeTouchTimeUs();

  // Contacts must not straddle engines within one batch: if a new contact
  // lands on a different surface, flush what belongs to the previous engine
  // first.
  Engine* engine = d->m_surface_engine_map[surface];
  if (engine != d->m_touch_engine) {
    touch_flush_frame(d);
  }
  d->m_active_surface = surface;
  d->m_touch_engine = engine;

  d->m_touch.points[id] = {x_w, y_w};
  touch_push(d,
             {FlutterPointerPhase::kDown, wl_fixed_to_double(x_w),
              wl_fixed_to_double(y_w), id},
             ts_us);
}

void Display::touch_handle_up(void* data,
                              struct wl_touch* /* wl_touch */,
                              uint32_t /* serial */,
                              uint32_t /* time */,
                              int32_t id) {
  auto* d = static_cast<Display*>(data);
  // Take before the unknown-id early return: up carries a wl timestamp, so a
  // pending stamp must be consumed even for dropped events.
  const uint64_t ts_us = d->input_timestamps_.TakeTouchTimeUs();

  // wl_touch.up carries no coordinates; reuse the contact's last position.
  // Unknown id (no prior down seen, e.g. attach mid-session) is dropped —
  // the engine was never told this device went down.
  const auto it = d->m_touch.points.find(id);
  if (it == d->m_touch.points.end()) {
    return;
  }
  touch_push(d,
             {kUp, wl_fixed_to_double(it->second.first),
              wl_fixed_to_double(it->second.second), id},
             ts_us);
  d->m_touch.points.erase(it);
}

void Display::touch_handle_motion(void* data,
                                  struct wl_touch* /* wl_touch */,
                                  uint32_t /* time */,
                                  int32_t id,
                                  wl_fixed_t x_w,
                                  wl_fixed_t y_w) {
  auto* d = static_cast<Display*>(data);
  // Take before the unknown-id early return (see touch_handle_up).
  const uint64_t ts_us = d->input_timestamps_.TakeTouchTimeUs();

  const auto it = d->m_touch.points.find(id);
  if (it == d->m_touch.points.end()) {
    return;
  }
  it->second = {x_w, y_w};
  touch_push(d,
             {FlutterPointerPhase::kMove, wl_fixed_to_double(x_w),
              wl_fixed_to_double(y_w), id},
             ts_us);
}

void Display::touch_handle_cancel(void* data, struct wl_touch* /* wl_touch */) {
  auto* d = static_cast<Display*>(data);
  SPDLOG_DEBUG("touch_handle_cancel");

  // The compositor cancelled the whole touch session (e.g. it recognized a
  // system gesture). Every active contact must be cancelled in the engine —
  // a contact left in the down phase trips
  // FML_DCHECK(!state.is_down) in the engine's PointerDataPacketConverter on
  // the next session's down for that device, and leaks its gesture-arena
  // entry in the framework. Cancel each contact at its last known position
  // (the previous code cancelled only device 0, at the *mouse* position).
  d->m_touch.frame.clear();  // pending updates for this session are moot
  // The synthesized cancels have no hardware moment; drop any latched
  // high-res stamp so the flush uses arrival time (cancel itself carries no
  // wl timestamp, so no stamp is pending for it).
  d->m_touch.frame_time_us = 0;
  for (const auto& [id, pos] : d->m_touch.points) {
    d->m_touch.frame.push_back({kCancel, wl_fixed_to_double(pos.first),
                                wl_fixed_to_double(pos.second), id});
  }
  d->m_touch.points.clear();
  // The protocol does not guarantee a frame event after cancel; flush now.
  touch_flush_frame(d);
}

void Display::touch_handle_frame(void* data, struct wl_touch* /* wl_touch */) {
  touch_flush_frame(static_cast<Display*>(data));
}

void Display::touch_flush_frame(Display* d) {
  if (d->m_touch.frame.empty()) {
    return;
  }
  if (d->m_touch_engine) {
    // One motion-to-photon sample per touch frame (contacts share the frame's
    // hardware scan timestamp), not one per contact.
    d->NoteInput(d->m_touch.frame_time_us);
    d->m_touch_engine->CoalesceTouchFrame(d->m_touch.frame.data(),
                                          d->m_touch.frame.size(),
                                          d->m_touch.frame_time_us);
  }
  d->m_touch.frame.clear();
  d->m_touch.frame_time_us = 0;
}

void Display::touch_push(Display* d,
                         const Engine::TouchEvent& ev,
                         const uint64_t ts_us) {
  // Latch the frame's shared stamp from the first contact event that carried
  // one (contacts in a frame are one hardware scan; see touch_.frame_time_us).
  if (d->m_touch.frame_time_us == 0) {
    d->m_touch.frame_time_us = ts_us;
  }
  d->m_touch.frame.push_back(ev);
  // wl_touch.frame is the normal flush boundary, but the protocol can't force
  // a compositor to send it; cap the batch so a stream without frame markers
  // can't grow the accumulator unbounded (parity with the drm/software seats).
  if (d->m_touch.frame.size() >= static_cast<size_t>(kMaxPointerEvent)) {
    touch_flush_frame(d);
  }
}

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

void Display::ArmWaylandRead() {
  // Canonical libwayland + reactor integration: prepare a read on the calling
  // (reactor) thread, flush outbound requests, then wait for the display fd to
  // become readable. The handler reads + dispatches and re-arms itself, so the
  // io_context never returns from run() until the descriptor is cancelled.
  while (wl_display_prepare_read(m_display) != 0) {
    wl_display_dispatch_pending(m_display);
  }
  wl_display_flush(m_display);

  wl_fd_->async_wait(asio::posix::stream_descriptor::wait_read,
                     [this](const std::error_code& ec) {
                       if (ec) {
                         // Cancelled (teardown) — undo the pending
                         // prepare_read.
                         wl_display_cancel_read(m_display);
                         return;
                       }
                       wl_display_read_events(m_display);
                       wl_display_dispatch_pending(m_display);
                       ArmWaylandRead();
                     });
}

void Display::ArmRepeatFd() {
  // ioc_ is wired by App after construction; the constructor's seat enumeration
  // can create the keyboard before then, so defer arming until StartEvents.
  if (!ioc_ || keyboard_.IsNull()) {
    return;
  }
  const int fd = keyboard_.Get()->GetRepeatFd();
  if (fd < 0) {
    return;
  }
  // The timerfd is owned by the keyboard handler; the descriptor must not close
  // it (released in ReleaseRepeatFd). Registered on the App-owned reactor.
  repeat_fd_.emplace(*ioc_, fd);
  ArmRepeatWait();
}

void Display::ArmRepeatWait() {
  repeat_fd_->async_wait(asio::posix::stream_descriptor::wait_read,
                         [this](const std::error_code& ec) {
                           if (ec) {
                             return;
                           }
                           // Re-resolves keysym/modifiers from the handler's
                           // xkb_state on this (reactor) thread, then re-arms.
                           if (!keyboard_.IsNull()) {
                             keyboard_.Get()->DispatchRepeat();
                           }
                           ArmRepeatWait();
                         });
}

void Display::ReleaseRepeatFd() {
  if (!repeat_fd_) {
    return;
  }
  std::error_code ec;
  repeat_fd_->cancel(ec);
  repeat_fd_->release();  // detach without closing the handler's timerfd
  repeat_fd_.reset();
}

void Display::ReleaseWaylandFd() {
  if (!wl_fd_) {
    return;
  }
  std::error_code ec;
  wl_fd_->cancel(ec);
  wl_fd_->release();  // detach without closing libwayland's display fd
  wl_fd_.reset();
}

void Display::StartEvents() {
  if (wl_fd_) {
    return;  // already armed
  }
  // App must have wired the shared reactor first.
  assert(ioc_ && "Display::SetEventLoop must precede StartEvents");
  // Registry/seat enumeration already ran (synchronously) in the constructor;
  // arm the display fd on the App-owned reactor. Construct the descriptor over
  // the wl fd (libwayland owns it; released in ~Display / StopEvents).
  wl_fd_.emplace(*ioc_, wl_display_get_fd(m_display));
  ArmWaylandRead();
  // A keyboard bound during the constructor's seat enumeration deferred its
  // repeat-fd arming (no reactor yet); arm it now that ioc_ is wired.
  if (!keyboard_.IsNull()) {
    ArmRepeatFd();
  }
}

void Display::StopEvents() {
  // App owns and stops the reactor; this connection only detaches its own
  // descriptors from the (libwayland / handler) fds. Called from ~App after the
  // reactor has stopped, so no concurrent reactor access — cancel + release
  // directly (release() never closes the borrowed fds).
  ReleaseRepeatFd();
  ReleaseWaylandFd();
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

void Display::NotifyOutputScaleChanged(const output_info_t* oi) {
  const size_t idx = IndexOfOutput(oi);
  if (idx >= m_all_outputs.size()) {
    return;
  }
  const int32_t scale = GetBufferScale(static_cast<uint32_t>(idx));
  std::vector<WaylandWindow*> snapshot;
  {
    std::lock_guard lock(m_windows_lock);
    snapshot = m_windows;
  }
  for (auto* w : snapshot) {
    w->OnOutputScaleChanged(idx, scale);
  }
}

wl_proxy* Display::CreateViewport(struct wl_surface* surface) const {
  namespace vp = viewporter::client;
  if (m_viewporter == nullptr || surface == nullptr) {
    return nullptr;
  }
  return wl_proxy_marshal_constructor(
      m_viewporter, vp::wp_viewporter_traits::Op::GetViewport,
      &vp::wp_viewport_traits::wl_iface(), nullptr, surface);
}

wl_proxy* Display::CreateFractionalScale(struct wl_surface* surface) const {
  namespace fs = fractional_scale_v1::client;
  if (m_fractional_scale_manager == nullptr || surface == nullptr) {
    return nullptr;
  }
  return wl_proxy_marshal_constructor(
      m_fractional_scale_manager,
      fs::wp_fractional_scale_manager_v1_traits::Op::GetFractionalScale,
      &fs::wp_fractional_scale_v1_traits::wl_iface(), nullptr, surface);
}

size_t Display::GetOutputIndexByHandle(struct wl_output* output) const {
  for (size_t i = 0; i < m_all_outputs.size(); ++i) {
    if (m_all_outputs[i]->output == output) {
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

homescreen::OutputInfo Display::ToOutputInfo(const output_info_t& oi) {
  homescreen::OutputInfo info;
  info.name = oi.name;
  info.width_px = static_cast<int32_t>(oi.width);
  info.height_px = static_cast<int32_t>(oi.height);
  info.mm_width = static_cast<int32_t>(oi.physical_width);
  info.mm_height = static_cast<int32_t>(oi.physical_height);
  info.refresh_hz = oi.refresh_rate;
  // A wl_output a client can see is by definition live. Wayland exposes no EDID
  // serial to clients, and make/model are not retained here.
  info.connected = true;
  info.handle = reinterpret_cast<uint64_t>(oi.output);
  return info;
}

std::vector<homescreen::OutputInfo> Display::EnumerateOutputs() const {
  std::vector<homescreen::OutputInfo> outputs;
  outputs.reserve(m_all_outputs.size());
  for (const auto& oi : m_all_outputs) {
    if (oi && oi->done) {
      outputs.push_back(ToOutputInfo(*oi));
    }
  }
  return outputs;
}

void Display::SetOutputListener(homescreen::IOutputListener* listener) {
  m_output_listener = listener;
}

void Display::EmitOutputDone(output_info_t* oi) {
  if (m_output_listener == nullptr || oi == nullptr) {
    return;
  }
  if (oi->announced) {
    m_output_listener->OnOutputChanged(ToOutputInfo(*oi));
  } else {
    oi->announced = true;
    m_output_listener->OnOutputAdded(ToOutputInfo(*oi));
  }
}

void Display::HandleGlobalRemove(const uint32_t global_name) {
  // global_remove fires for every global; act only on a matching wl_output.
  for (auto it = m_all_outputs.begin(); it != m_all_outputs.end(); ++it) {
    const auto& oi = *it;
    if (!oi || oi->global_id != global_name) {
      continue;
    }
    if (m_output_listener != nullptr && oi->announced) {
      m_output_listener->OnOutputRemoved(oi->name);
    }
    m_all_outputs.erase(it);
    return;
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
