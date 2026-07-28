/*
 * Copyright 2026 Toyota Connected North America
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

#include "backend/register_backends.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "backend/backend.h"
#include "config/common.h"
#include "configuration/configuration.h"
#include "display/idisplay.h"
#include "display/output_manager.h"
#include "logging/logging.h"

// Independent guards so several backends can be compiled into one binary. The
// bodies below are the verbatim branch bodies that used to live in the
// App::MakeDisplay and Backend::Create #if/#elif chains.
#if BUILD_BACKEND_DRM_KMS_EGL
#include "backend/drm_kms_egl/drm_backend.h"
#include "backend/drm_kms_egl/drm_cursor.h"  // DrmCursor -> ICursorShapeSink*
#include "backend/drm_kms_egl/gl_cursor.h"   // GlCursor -> ICursorShapeSink*
#include "display/drm_display.h"
#endif
#if BUILD_BACKEND_DRM_KMS_VULKAN
#include "backend/drm_kms_egl/drm_cursor.h"  // DrmCursor -> ICursorShapeSink*
#include "backend/drm_kms_vulkan/vulkan_drm_backend.h"
#include "display/drm_display.h"
#endif
#if BUILD_BACKEND_WAYLAND_LEASED_DRM
#include "backend/wayland_leased_drm/lease_client.h"
#endif
#if BUILD_BACKEND_DRM_KMS_EGL || BUILD_BACKEND_DRM_KMS_VULKAN
#include "display/drm_device_resolver.h"  // ResolveDrmDevice
#include "display/drm_mode_list.h"        // PrintDrmModes for --drm-list-modes
#endif
#if BUILD_BACKEND_SOFTWARE
#if BUILD_SOFTWARE_SINK_DRM
#include "backend/software/drm_dumb_sink.h"  // PrintDumbSinkModes
#endif
#include "backend/software/sink_factory.h"
#include "backend/software/software_backend.h"
#include "backend/software/software_cursor.h"
#include "display/software_display.h"
#if BUILD_SOFTWARE_INPUT_LIBINPUT
#include "backend/software/input/software_seat.h"
#endif
#endif
#if BUILD_BACKEND_HEADLESS_EGL
#include "backend/headless_egl/headless_egl.h"
#include "backend/software/nv12_consumer.h"  // MakeNv12Consumer
#include "display/software_display.h"        // reused no-op IDisplay
#endif
#if BUILD_BACKEND_WAYLAND_EGL
#include "backend/wayland_egl/partial_repaint_gate.h"
#include "backend/wayland_egl/wayland_egl.h"
#endif
#if BUILD_BACKEND_WAYLAND_VULKAN
#include "backend/wayland_vulkan/wayland_vulkan.h"
#endif
#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
#include "wayland/display.h"
#endif

namespace {

#if BUILD_BACKEND_DRM_KMS_EGL || BUILD_BACKEND_DRM_KMS_VULKAN
// DRM/KMS does not have a compositor-level display concept. The refresh
// rate and mode are owned by the backend; the DrmDisplay stub answers
// queries the shell issues (metrics, cursor activation, event loop) with
// safe defaults. Backend-side hooks can refine the refresh rate later. Shared
// by both DRM backends (they differ only in renderer).
std::shared_ptr<IDisplay> MakeDrmDisplay(
    const std::vector<Configuration::Config>& configs) {
  const auto w = configs[0].view.width.value_or(kDefaultViewWidth);
  const auto h = configs[0].view.height.value_or(kDefaultViewHeight);
  const bool no_seat = configs[0].view.drm_no_seat.value_or(false);
  // Resolve the card once here; the DRM backend reads it back from the display
  // (device_path()) so both agree. ResolveDrmDevice logs the choice / the
  // reason it could not pick one.
  auto device = homescreen::ResolveDrmDevice(configs[0].view.drm_device);
  if (!device) {
    std::exit(EXIT_FAILURE);
  }
  return std::make_shared<DrmDisplay>(static_cast<int32_t>(w),
                                      static_cast<int32_t>(h), 60.0,
                                      std::move(*device), no_seat);
}
#endif

#if BUILD_BACKEND_WAYLAND_LEASED_DRM
// Build the LeaseConfig shared by every leased tier, including the revocation
// policy.
//
// `exit` is the default because the alternative today is worse than it sounds:
// gating leaves a frozen panel with no signal to anyone, and revocation is
// routine — every VT switch away from the compositor is one. Exiting non-zero
// hands the problem to the supervisor that can actually fix it, which
// renegotiates from scratch on restart. Reacquire-in-place is later and will
// change this default per tier.
homescreen::LeaseConfig MakeLeaseConfig(const Configuration::Config& config) {
  homescreen::LeaseConfig lc;
  lc.device = config.view.lease_device;
  lc.connector = config.view.lease_connector;
  if (config.view.lease_timeout_ms.has_value()) {
    lc.timeout_ms = *config.view.lease_timeout_ms;
  }

  const std::string policy = config.view.lease_on_revoke.value_or("exit");
  if (policy == "gate") {
    // Null handler: revoked() latches, the renderers' gates fire, and the panel
    // stops updating. Debugging aid — nothing recovers.
    ihs::log::warn(
        "[leased-drm] lease_on_revoke=gate: on revocation the panel will "
        "freeze "
        "and the process will keep running. Nothing recovers from this; it is "
        "for observing a revocation, not for production.");
    return lc;
  }
  if (policy != "exit") {
    ihs::log::warn(
        "[leased-drm] lease_on_revoke='{}' is not implemented (accepted "
        "values: "
        "exit, gate); using 'exit'.",
        policy);
  }

  lc.on_revoked = [](homescreen::RevokeCause cause) {
    // Runs on the monitor thread. std::exit would run atexit handlers and
    // static destructors concurrently with the threads still using them, so
    // _Exit: the kernel closes the lease fd, which is what actually returns the
    // connector, and the compositor re-advertises it. LatchRevoked has already
    // logged the cause in operator terms; this says what is being done about
    // it.
    ihs::log::critical(
        "[leased-drm] exiting on lease revocation ({}). lease_on_revoke=exit: "
        "a "
        "supervisor should restart and renegotiate. Use lease_on_revoke=gate "
        "to "
        "keep the process alive with a frozen panel instead.",
        homescreen::ToString(cause));
    std::_Exit(EXIT_FAILURE);
  };
  return lc;
}
#endif

#if BUILD_BACKEND_WAYLAND_LEASED_DRM
// Acquire the lease or exit. Shared by every leased tier's make_display: the
// failure contract must not drift between tiers, and it is one contract --
// a leased backend that could not get its lease has nothing to fall back to,
// and silently running on a different device would be worse than not starting.
// Fail-fast is consistent with the rest of MakeDisplay.
std::shared_ptr<homescreen::LeaseHold> AcquireLeaseOrExit(
    const Configuration::Config& config) {
  auto res = homescreen::LeaseClient::Acquire(MakeLeaseConfig(config));
  if (!res.hold) {
    ihs::log::error("[leased-drm] could not acquire a lease: {}",
                    homescreen::ToString(res.error));
    for (const auto& o : res.offers) {
      ihs::log::error("[leased-drm]   offered: {} (connector_id {}) — {}",
                      o.name, o.connector_id, o.description);
    }
    std::exit(EXIT_FAILURE);
  }
  return std::shared_ptr<homescreen::LeaseHold>(std::move(res.hold));
}
#endif

#if BUILD_BACKEND_WAYLAND_LEASED_DRM && \
    (BUILD_BACKEND_DRM_KMS_EGL || BUILD_BACKEND_DRM_KMS_VULKAN)
// wayland-leased-drm: the DRM device comes from a compositor lease rather than
// an open() of a card we chose. Everything downstream is the same DrmDisplay
// the direct backends use -- the lease only changes the acquisition axis, not
// the renderer -- so the DRM backend factories are reused verbatim.
std::shared_ptr<IDisplay> MakeLeasedDrmDisplay(
    const std::vector<Configuration::Config>& configs) {
  // The display owns the hold: drm::Device::from_fd only borrows the fd, so the
  // LeaseHold (which owns it, and whose destruction returns the lease) must
  // outlive every backend built on it.
  const std::shared_ptr<homescreen::LeaseHold> hold =
      AcquireLeaseOrExit(configs[0]);
  const int fd = hold->fd();
  std::string card = hold->card_path();

  const auto w = configs[0].view.width.value_or(kDefaultViewWidth);
  const auto h = configs[0].view.height.value_or(kDefaultViewHeight);
  // connector_id and the revocation gate ride along to the backend through the
  // display -- the DRM backend factories take everything else from it too. The
  // gate captures the hold by value: fd_owner_ holds the same share, so the
  // callable cannot outlive what it reads.
  return std::make_shared<DrmDisplay>(
      DrmDisplay::AdoptFd{}, static_cast<int32_t>(w), static_cast<int32_t>(h),
      60.0, fd, std::move(card), hold, hold->connector_id(),
      [hold] { return hold->revoked(); });
}
#endif

#if BUILD_BACKEND_WAYLAND_EGL || BUILD_BACKEND_WAYLAND_VULKAN
// The Wayland compositor connection. Shared by both Wayland backends.
std::shared_ptr<IDisplay> MakeWaylandDisplay(
    const std::vector<Configuration::Config>& configs) {
  return std::make_shared<Display>(!configs[0].disable_cursor,
                                   configs[0].wayland_event_mask,
                                   configs[0].cursor_theme, configs);
}
#endif

#if BUILD_BACKEND_SOFTWARE
// No compositor, no Wayland, no DRM — just an IDisplay that owns
// (a) a refresh-rate denominator for App::Loop and (b) an optional
// libinput-backed seat. 60 Hz default; a sink with a real vblank
// can override later.
std::shared_ptr<IDisplay> MakeSoftwareDisplay(
    const std::vector<Configuration::Config>& configs) {
  const auto w = configs[0].view.width.value_or(kDefaultViewWidth);
  const auto h = configs[0].view.height.value_or(kDefaultViewHeight);
  auto display = std::make_shared<SoftwareDisplay>(
      static_cast<int32_t>(w), static_cast<int32_t>(h), 60.0);
#if BUILD_SOFTWARE_INPUT_LIBINPUT
  // IVI_SW_INPUT controls whether the seat is wired:
  //   "none"     — skip; engine runs without input (CI default).
  //   anything else (including unset / "libinput" / "auto") — wire
  //   the libinput seat.
  // Default-wire matches operator expectations on device targets and
  // is a no-op on CI hosts that lack /dev/input/event* anyway.
  const char* mode = std::getenv("IVI_SW_INPUT");
  const bool want_input = mode == nullptr || std::string_view(mode) != "none";
  if (want_input) {
    display->SetSeat(std::make_unique<homescreen::SoftwareSeat>(
        static_cast<int32_t>(w), static_cast<int32_t>(h)));
  } else {
    ihs::log::info("[SoftwareBackend] IVI_SW_INPUT=none — no input seat");
  }
#endif
  // Software cursor, gated by --disable-cursor / IVI_SW_CURSOR=0. Owned by the
  // display; shared with the seat (updates its position on motion) and the sink
  // (composites it). Harmless for headless sinks — they ignore SetCursor, and
  // the cursor stays invisible until the first pointer motion.
  const char* cursor_env = std::getenv("IVI_SW_CURSOR");
  const bool cursor_enabled =
      !configs[0].disable_cursor &&
      (cursor_env == nullptr || std::string_view(cursor_env) != "0");
  if (cursor_enabled) {
    display->SetCursor(std::make_shared<SoftwareCursor>());
  }
  return display;
}
#endif

#if BUILD_BACKEND_WAYLAND_LEASED_DRM && BUILD_BACKEND_SOFTWARE && \
    BUILD_SOFTWARE_SINK_DRM
// Negotiate a lease, then hand the fd to a SoftwareDisplay for the backend to
// build its dumb sink on. Shares the seat/cursor wiring with the direct
// software display; only the scanout device differs.
std::shared_ptr<IDisplay> MakeLeasedSoftwareDisplay(
    const std::vector<Configuration::Config>& configs) {
  const std::shared_ptr<homescreen::LeaseHold> hold =
      AcquireLeaseOrExit(configs[0]);

  auto display = MakeSoftwareDisplay(configs);
  auto* sw = dynamic_cast<SoftwareDisplay*>(display.get());
  assert(sw != nullptr);
  SoftwareDisplay::LeasedScanout scanout;
  scanout.fd = hold->fd();
  scanout.connector_id = hold->connector_id();
  // By value: the sink polls this for the process lifetime, so it must not
  // capture anything with a shorter life than the hold it reads.
  scanout.revoked = [hold]() { return hold->revoked(); };
  scanout.owner = hold;
  sw->SetLeasedScanout(std::move(scanout));
  return display;
}
#endif

#if BUILD_BACKEND_DRM_KMS_EGL
std::shared_ptr<Backend> MakeDrmEglBackend(const Configuration::Config& config,
                                           IDisplay* display) {
  std::shared_ptr<Backend> m_backend;
  auto parse_tri = [](const std::optional<std::string>& s,
                      drm_config::TriState def = drm_config::TriState::kAuto) {
    if (!s.has_value() || s->empty() || *s == "auto") {
      return def;
    }
    if (*s == "yes" || *s == "true" || *s == "on") {
      return drm_config::TriState::kYes;
    }
    if (*s == "no" || *s == "false" || *s == "off") {
      return drm_config::TriState::kNo;
    }
    ihs::log::warn("[FlutterView] drm tri-state '{}' unrecognized; using auto",
                   *s);
    return drm_config::TriState::kAuto;
  };
  auto parse_compositor = [](const std::optional<std::string>& s) {
    if (!s.has_value() || s->empty() || *s == "auto") {
      return drm_config::Compositor::kAuto;
    }
    if (*s == "planes") {
      return drm_config::Compositor::kPlanes;
    }
    if (*s == "gl") {
      return drm_config::Compositor::kGl;
    }
    ihs::log::warn("[FlutterView] drm-compositor '{}' unrecognized; using auto",
                   *s);
    return drm_config::Compositor::kAuto;
  };
  auto parse_modeset = [](const std::optional<std::string>& s) {
    if (!s.has_value() || s->empty() || *s == "auto") {
      return drm_config::Modeset::kAuto;
    }
    if (*s == "legacy") {
      return drm_config::Modeset::kLegacy;
    }
    if (*s == "atomic") {
      return drm_config::Modeset::kAtomic;
    }
    ihs::log::warn("[FlutterView] drm-modeset '{}' unrecognized; using auto",
                   *s);
    return drm_config::Modeset::kAuto;
  };
  auto parse_format = [](const std::optional<std::string>& s) {
    if (!s.has_value() || s->empty() || *s == "auto") {
      return drm_config::PrimaryFormat::kAuto;
    }
    if (*s == "xrgb8888") {
      return drm_config::PrimaryFormat::kXrgb8888;
    }
    if (*s == "xbgr8888") {
      return drm_config::PrimaryFormat::kXbgr8888;
    }
    if (*s == "argb8888") {
      return drm_config::PrimaryFormat::kArgb8888;
    }
    if (*s == "abgr8888") {
      return drm_config::PrimaryFormat::kAbgr8888;
    }
    if (*s == "rgb565") {
      return drm_config::PrimaryFormat::kRgb565;
    }
    ihs::log::warn(
        "[FlutterView] drm-primary-format '{}' unrecognized "
        "(expected auto|xrgb8888|xbgr8888|argb8888|abgr8888|rgb565); "
        "using auto",
        *s);
    return drm_config::PrimaryFormat::kAuto;
  };

  // --fullscreen on DRM = drive the panel at its preferred mode with no
  // framing. Clearing width/height here lets DrmBackend fall through to
  // cfg_.width.value_or(mode_.hdisplay), i.e. the native mode. When -f
  // is combined with explicit width/height, -f wins — matches how most
  // CLI tools handle a scalar "use native" flag vs. explicit sizing.
  // DrmDisplay (built first by App::BuildDisplays) resolved and owns the card;
  // read it back so the backend and display target the same device. In a DRM
  // build MakeDrmDisplay always returns a DrmDisplay, so the cast fails only on
  // programmer error.
  auto* drm_display = dynamic_cast<DrmDisplay*>(display);
  assert(drm_display != nullptr);

  const bool fullscreen = config.view.fullscreen.value_or(false);
  DrmConfig cfg{
      drm_display->device_path(),
      fullscreen ? std::optional<uint32_t>{} : config.view.width,
      fullscreen ? std::optional<uint32_t>{} : config.view.height,
      config.debug_backend.value_or(false),
  };
  cfg.disable_cursor = config.disable_cursor.value_or(false);
  cfg.cursor_theme = config.cursor_theme;
  // Treat empty TOML/env/CLI string as "unset" — operator-friendly:
  // `drm_connector = ""` in TOML shouldn't force a strict empty-name
  // match against zero connectors.
  // [view.output] (validated against the card's live connectors via the output
  // provider) selects the connector; fall back to the raw [view.backend.drm]
  // connector when no [view.output] match applies.
  if (auto resolved = homescreen::OutputManager::ResolveForView(
          config, display, homescreen::BackendFamily::kDrm)) {
    cfg.connector_name = std::move(resolved);
  } else if (config.view.drm_connector.has_value() &&
             !config.view.drm_connector->empty()) {
    cfg.connector_name = config.view.drm_connector;
  }
  if (config.view.drm_mode.has_value() && !config.view.drm_mode->empty()) {
    cfg.mode_spec = config.view.drm_mode;
  }
  cfg.compositor = parse_compositor(config.view.drm_compositor);
  cfg.modeset = parse_modeset(config.view.drm_modeset);
  cfg.allow_nonblock_modeset =
      parse_tri(config.view.drm_allow_nonblock_modeset);
  cfg.primary_format = parse_format(config.view.drm_primary_format);
  cfg.overlay_planes = parse_tri(config.view.drm_overlay_planes);
  cfg.explicit_sync = parse_tri(config.view.drm_explicit_sync);
  cfg.async_flip = parse_tri(config.view.drm_async_flip);
  // kAuto (default) resolves per driver in DrmBackend::Create — staging is
  // enabled only on nvidia-drm; yes/no force the choice.
  cfg.stage_cursor = parse_tri(config.view.drm_stage_cursor);
  // --drm-no-seat: DrmDisplay forced the session null; tell DrmBackend to
  // also skip the foreground-VT guard on its direct-open path.
  cfg.no_seat = config.view.drm_no_seat.value_or(false);
  // Leased tier: the compositor chose the connector, and the lease can be
  // revoked under us. Both are inert (0 / empty) on the direct tiers, which is
  // what makes this factory serve the leased descriptor unchanged.
  cfg.lease_connector_id = drm_display->lease_connector_id();
  cfg.lease_revoked = drm_display->lease_revoked();

  // drm_display (resolved above) owns the process-wide libseat session — it may
  // be null when no seat backend is available, in which case DrmBackend takes
  // the legacy direct-open path.
  // The device-context (DrmDisplay) owns the card open + master; every backend
  // scanning out to this card shares it. Acquired lazily here on first use.
  m_backend = DrmBackend::Create(cfg, drm_display->session(),
                                 drm_display->SharedDevice(), drm_display);

  // DrmBackend::Create returns nullptr on any init failure (libseat
  // take_device, drmSetMaster, no usable connector, GBM/EGL setup,
  // …). Continuing would dereference a null backend in Engine::Run
  // and SEGV; fail-fast with the same exit path as a missing bundle.
  if (!m_backend) {
    ihs::log::critical("[FlutterView] DRM backend init failed; aborting");
    exit(EXIT_FAILURE);
  }

  // Start the card's single PAGE_FLIP_EVENT reader (owned by the device-context
  // so it outlives every backend on the card). The dispatcher routes each flip
  // to the committing backend by user_data, so N views on one card share one
  // reader. Idempotent across backends on the same card.
  drm_display->SetFlipHandler(&DrmBackend::UnifiedPageFlipHandler);
  drm_display->StartFlipReader();

  // Wire the HW cursor (if Create succeeded in opening one) to the
  // seat dispatch thread so pointer events move the on-screen sprite.
  // The pointer stays valid for the FlutterView's lifetime — both
  // m_backend and drm_display are members destroyed in declaration
  // order during ~FlutterView.
  if (auto* drm_backend = dynamic_cast<DrmBackend*>(m_backend.get());
      drm_backend != nullptr) {
    // Push the resolved framebuffer dimensions into the seat's
    // cursor-clamp viewport. App::MakeDisplay seeds DrmDisplay with
    // the config's view.width/height (defaults 1024x768) before the
    // backend has resolved the actual mode — `-f` then promotes the
    // FB to the full mode dimensions, leaving the seat's pointer
    // clamp stuck at the config size unless we update it here.
    drm_display->SetViewportSize(static_cast<int32_t>(drm_backend->width()),
                                 static_cast<int32_t>(drm_backend->height()));
    // Place this view in the combined pointer space so, when several views
    // share the card's seat, the pointer routes to the display it is over.
    drm_display->SetRegionLayout(config.view.output.layout_x.value_or(0),
                                 config.view.output.layout_y.value_or(0));
    if (config.view.output.touch_device.has_value()) {
      drm_display->SetRegionTouchDevice(*config.view.output.touch_device);
    }
    drm_display->SetCursor(drm_backend->drm_cursor());
    // Composited cursor fallback (non-null only when no HW cursor plane).
    drm_display->SetGlCursor(drm_backend->gl_cursor());
    // Retarget the cursor sprite on activateSystemCursor: the HW cursor when
    // a cursor plane exists, else the GL-composited fallback. gl_cursor() is
    // typed as ICursorPositionSink*; cross-cast to the sibling shape-sink
    // interface (GlCursor implements both).
    if (auto* hw = drm_backend->drm_cursor()) {
      drm_display->SetCursorShapeSink(hw);
    } else {
      drm_display->SetCursorShapeSink(
          dynamic_cast<homescreen::ICursorShapeSink*>(
              drm_backend->gl_cursor()));
    }

    // this one engine also drives the additional outputs listed in
    // [[view.output]] (entries 2..N). Each becomes a compositor on its own CRTC
    // (sharing this backend's EGL/GBM) bound to view id 1..N; FlutterView
    // FlutterEngineAddView's them once the engine is running.
    int64_t next_view_id = 1;
    for (const auto& extra : config.view.additional_outputs) {
      const std::string connector = extra.drm_connector.value_or("");
      if (connector.empty()) {
        ihs::log::warn(
            "[FlutterView] additional [[view.output]] has no drm_connector; "
            "skipping");
        continue;
      }
      if (drm_backend->AddOutput(connector, next_view_id)) {
        ++next_view_id;
      }
    }
  }
  return m_backend;
}
#endif

#if BUILD_BACKEND_DRM_KMS_VULKAN
std::shared_ptr<Backend> MakeDrmVulkanBackend(
    const Configuration::Config& config,
    IDisplay* display) {
  std::shared_ptr<Backend> m_backend;
  // Shares DrmDisplay (libseat session, refresh rate, cursor) with the EGL
  // DRM backend; only the renderer differs. App::MakeDisplay always builds a
  // DrmDisplay in a DRM build, so the cast failing is programmer error.
  auto* drm_display = dynamic_cast<DrmDisplay*>(display);
  assert(drm_display != nullptr);
  const std::string drm_mode =
      (config.view.drm_mode.has_value() && !config.view.drm_mode->empty())
          ? *config.view.drm_mode
          : std::string{};
  // Serves both the direct and the leased descriptor: the lease changes only
  // where the DRM fd came from. Unlike MakeDrmEglBackend — which is reused
  // verbatim because DrmBackend::Create takes the device from the display —
  // VulkanDrmBackend::Create takes a path, so the branch lives here rather than
  // being invisible.
  //
  // device_path() is passed on both paths: on a lease it was derived from the
  // lease fd and is only ever stat()ed, for the physical-device match. The fd
  // itself is what gets opened/adopted.
  auto vk_backend =
      drm_display->adopted_fd()
          ? VulkanDrmBackend::Create(
                drm_display->SharedDevice()->fd(), drm_display->fd_owner(),
                drm_display->device_path(),
                config.debug_backend.value_or(false), drm_mode,
                config.view.drm_rotation.value_or(0),
                drm_display->lease_connector_id(), drm_display->lease_revoked())
          : VulkanDrmBackend::Create(drm_display->device_path(),
                                     config.debug_backend.value_or(false),
                                     drm_display->session(), drm_mode,
                                     config.view.drm_rotation.value_or(0));

  // Create returns nullptr on any init failure (unsupported device, no
  // zero-copy scanout path). Continuing would dereference a null backend in
  // Engine::Run and SEGV; fail-fast with the same exit path the EGL DRM
  // backend uses.
  if (!vk_backend) {
    ihs::log::critical(
        "[FlutterView] DRM Vulkan backend init failed; aborting");
    exit(EXIT_FAILURE);
  }

  // Wire the self-committing HW cursor (if Create opened one) to the seat
  // dispatch thread, and clamp the pointer to the resolved scanout size. The
  // cursor commits on its own (not staged into the per-frame scanout commit),
  // so it stays responsive while the UI is idle. The pointer stays valid for
  // the FlutterView's lifetime (both members, destroyed in declaration
  // order).
  drm_display->SetViewportSize(static_cast<int32_t>(vk_backend->width()),
                               static_cast<int32_t>(vk_backend->height()));
  // Place this view in the combined pointer space so, when several views share
  // the card's seat, the pointer routes to the display it is over.
  drm_display->SetRegionLayout(config.view.output.layout_x.value_or(0),
                               config.view.output.layout_y.value_or(0));
  if (config.view.output.touch_device.has_value()) {
    drm_display->SetRegionTouchDevice(*config.view.output.touch_device);
  }
  drm_display->SetCursor(vk_backend->drm_cursor());
  drm_display->SetCursorShapeSink(vk_backend->drm_cursor());
  drm_display->SetCursorRotation(vk_backend->rotation());
  drm_display->SetInputTransforms(config.view.drm_input_transforms);
  m_backend = vk_backend;
  return m_backend;
}
#endif

#if BUILD_BACKEND_WAYLAND_EGL
std::shared_ptr<Backend> MakeWaylandEglBackend(
    const Configuration::Config& config,
    IDisplay* display) {
  auto* wl = dynamic_cast<Display*>(display);
  assert(wl != nullptr);
  return std::make_shared<WaylandEglBackend>(
      wl, wl->GetDisplay(), config.view.width.value_or(kDefaultViewWidth),
      config.view.height.value_or(kDefaultViewHeight),
      config.debug_backend.value_or(false),
      EngineSwitchesEnableImpeller(config.view.engine_args), kEglBufferSize);
}
#endif

#if BUILD_BACKEND_WAYLAND_VULKAN
std::shared_ptr<Backend> MakeWaylandVulkanBackend(
    const Configuration::Config& config,
    IDisplay* display) {
  auto* wl = dynamic_cast<Display*>(display);
  assert(wl != nullptr);
  // Mesa's Vulkan WSI on Wayland needs zwp_linux_dmabuf_v1 (modern) or
  // wl_drm (legacy) to allocate GPU buffers; warn loudly if absent.
  if (!wl->HasLinuxDmabuf() && !wl->HasWlDrm()) {
    ihs::log::warn(
        "[WaylandVulkanBackend] compositor advertises neither "
        "zwp_linux_dmabuf_v1 nor wl_drm — Mesa Vulkan WSI cannot "
        "allocate swapchain images. Swapchain init will fail; fix on the "
        "compositor side (e.g. Weston needs --backend=drm-backend or its "
        "linux-dmabuf support built in).");
  }
  return std::make_shared<WaylandVulkanBackend>(
      wl, wl->GetDisplay(), config.view.width.value_or(kDefaultViewWidth),
      config.view.height.value_or(kDefaultViewHeight),
      config.debug_backend.value_or(false));
}
#endif

#if BUILD_BACKEND_SOFTWARE
std::shared_ptr<Backend> MakeSoftwareBackend(
    const Configuration::Config& config,
    IDisplay* display) {
  // The descriptor pairs make_display/make_backend, so the display is always a
  // SoftwareDisplay; a null cast is programmer error.
  auto* sw_display = dynamic_cast<SoftwareDisplay*>(display);
  assert(sw_display != nullptr);
  (void)sw_display;
  // Sink is picked at startup from IVI_SW_SINK. Default 'none' just
  // discards frames; 'memory' keeps the latest in-process; 'file:
  // <pattern>' writes PAM-format snapshots to disk.
  // --drm-device (config.view.drm_device) selects the card for the drm-dumb
  // sink, mirroring drm-kms-egl; empty lets the sink probe the first usable
  // card. An explicit IVI_SW_SINK=drm-dumb:/dev/dri/cardN still overrides.
  return std::make_shared<SoftwareBackend>(
      config.view.width.value_or(kDefaultViewWidth),
      config.view.height.value_or(kDefaultViewHeight),
      MakeSinkFromEnv(config.view.drm_device.value_or(std::string{})));
}
#endif

#if BUILD_BACKEND_HEADLESS_EGL
// A no-op display: the encode backend has no screen or input, only a
// refresh-rate denominator for App::Loop. Reuses SoftwareDisplay.
std::shared_ptr<IDisplay> MakeHeadlessEglDisplay(
    const std::vector<Configuration::Config>& configs) {
  const auto w = configs[0].view.width.value_or(kDefaultViewWidth);
  const auto h = configs[0].view.height.value_or(kDefaultViewHeight);
  return std::make_shared<SoftwareDisplay>(static_cast<int32_t>(w),
                                           static_cast<int32_t>(h), 60.0);
}

std::shared_ptr<Backend> MakeHeadlessEglBackend(
    const Configuration::Config& config,
    IDisplay* /*display*/) {
  // Consumer picked at startup from IVI_ENC_SINK (the part after "encoder:"):
  // file:<path> (default) or webrtc:<host>:<port>.
  const char* spec = std::getenv("IVI_ENC_SINK");
  auto consumer = MakeNv12Consumer(spec != nullptr ? spec : "file:out.h264");
  return std::make_shared<HeadlessEglBackend>(
      config.view.width.value_or(kDefaultViewWidth),
      config.view.height.value_or(kDefaultViewHeight), std::move(consumer));
}
#endif

#if BUILD_BACKEND_WAYLAND_LEASED_DRM && BUILD_BACKEND_SOFTWARE && \
    BUILD_SOFTWARE_SINK_DRM
std::shared_ptr<Backend> MakeLeasedSoftwareBackend(
    const Configuration::Config& config,
    IDisplay* display) {
  auto* sw_display = dynamic_cast<SoftwareDisplay*>(display);
  assert(sw_display != nullptr);
  const auto& scanout = sw_display->leased_scanout();
  // make_display fail-fasts when the lease cannot be had, so reaching here
  // without one is programmer error, not an operator mistake.
  assert(scanout.has_value());

  // Deliberately NOT MakeSinkFromEnv: the sink is not IVI_SW_SINK's to choose
  // once the backend key has pinned drm-dumb-on-a-leased-fd. IVI_SW_DRM_MODE
  // and the format/dither knobs still apply -- those say how to drive the
  // connector, which the lease does not change.
  auto sink = DrmDumbSink::Create(scanout->fd, scanout->owner,
                                  scanout->connector_id, scanout->revoked);
  if (!sink) {
    ihs::log::error(
        "[leased-drm] could not drive leased connector {} on the lease fd",
        scanout->connector_id);
    return nullptr;
  }
  return std::make_shared<SoftwareBackend>(
      config.view.width.value_or(kDefaultViewWidth),
      config.view.height.value_or(kDefaultViewHeight), std::move(sink));
}
#endif

}  // namespace

#if BUILD_BACKEND_DRM_KMS_EGL || BUILD_BACKEND_DRM_KMS_VULKAN
// --drm-list-modes for the DRM backends: list the KMS device's connector modes.
// An empty device defaults to card1.
static int DrmListModes(const std::string& device) {
  return PrintDrmModes(device.empty() ? "/dev/dri/card1" : device);
}
#endif
#if BUILD_BACKEND_SOFTWARE && BUILD_SOFTWARE_SINK_DRM
// --drm-list-modes for the software backend: list the dumb-sink modes. An empty
// device scans every /dev/dri/card* so the operator can discover the card.
// Only available when the DRM dumb sink is built; without it the software
// backend has no scanout device to enumerate.
static int SoftwareListModes(const std::string& device) {
  return PrintDumbSinkModes(device);
}
#endif

void RegisterCompiledBackends(backend::BackendRegistry& registry) {
#if BUILD_BACKEND_WAYLAND_EGL
  registry.Register(
      {"wayland-egl", MakeWaylandDisplay, MakeWaylandEglBackend, nullptr});
#endif
#if BUILD_BACKEND_WAYLAND_VULKAN
  registry.Register({"wayland-vulkan", MakeWaylandDisplay,
                     MakeWaylandVulkanBackend, nullptr});
#endif
#if BUILD_BACKEND_DRM_KMS_EGL
  registry.Register(
      {"drm-kms-egl", MakeDrmDisplay, MakeDrmEglBackend, DrmListModes});
#endif
#if BUILD_BACKEND_WAYLAND_LEASED_DRM && BUILD_BACKEND_DRM_KMS_EGL
  // Same renderer as drm-kms-egl, different acquisition: the fd is leased from
  // a compositor instead of opened directly. make_backend is reused as-is --
  // MakeDrmEglBackend takes the device from the display, and session() is null
  // on the adopted path, which DrmBackend::Create already supports.
  //
  // list_modes is deliberately null: --drm-list-modes on the direct backends
  // prints a whole card's connectors by path, which for a lease would present
  // inventory we cannot actually lease as though we could. A lease-aware
  // listing (LeaseClient::Probe) can back it later.
  registry.Register({"wayland-leased-drm-egl", MakeLeasedDrmDisplay,
                     MakeDrmEglBackend, nullptr});
#endif
#if BUILD_BACKEND_WAYLAND_LEASED_DRM && BUILD_BACKEND_DRM_KMS_VULKAN
  // Same display as the leased-egl tier (MakeLeasedDrmDisplay serves either DRM
  // renderer) and the same make_backend as the direct vulkan tier, which
  // branches on the display's adopted_fd(). Only the scanout/modeset half of
  // this backend touches the lease -- the VkDevice was never created from the
  // KMS fd.
  registry.Register({"wayland-leased-drm-vulkan", MakeLeasedDrmDisplay,
                     MakeDrmVulkanBackend, nullptr});
#endif
#if BUILD_BACKEND_WAYLAND_LEASED_DRM && BUILD_BACKEND_SOFTWARE && \
    BUILD_SOFTWARE_SINK_DRM
  // The cheapest tier: the engine renders to CPU memory and only the dumb
  // buffers plus CRTC state live on the leased fd. Unlike the direct software
  // backend this one does not consult IVI_SW_SINK -- the key already chose the
  // sink. list_modes is null for the same reason as the egl tier.
  registry.Register({"wayland-leased-drm-software", MakeLeasedSoftwareDisplay,
                     MakeLeasedSoftwareBackend, nullptr});
#endif
#if BUILD_BACKEND_DRM_KMS_VULKAN
  registry.Register(
      {"drm-kms-vulkan", MakeDrmDisplay, MakeDrmVulkanBackend, DrmListModes});
#endif
#if BUILD_BACKEND_SOFTWARE
  registry.Register({"software", MakeSoftwareDisplay, MakeSoftwareBackend,
#if BUILD_SOFTWARE_SINK_DRM
                     SoftwareListModes});
#else
                     nullptr});
#endif
#endif
#if BUILD_BACKEND_HEADLESS_EGL
  registry.Register({"headless-egl", MakeHeadlessEglDisplay,
                     MakeHeadlessEglBackend, nullptr});
#endif
}

// Picks which registered backend to run. With one backend it is that one; for a
// Wayland EGL+Vulkan build the view.backend hint ("vulkan" vs other) selects
// the renderer. Returns an empty string when nothing resolves (caller
// fail-fasts).
// A live Wayland session = WAYLAND_DISPLAY set AND the socket actually exists
// (the env var alone can be stale). WAYLAND_DISPLAY is either an absolute path
// or a name resolved under XDG_RUNTIME_DIR (matching libwayland).
static bool WaylandSessionAvailable() {
  const char* wl = std::getenv("WAYLAND_DISPLAY");
  if (wl == nullptr || *wl == '\0') {
    return false;
  }
  std::error_code ec;
  const std::filesystem::path sock(wl);
  if (sock.is_absolute()) {
    return std::filesystem::exists(sock, ec);
  }
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg == nullptr || *xdg == '\0') {
    return true;  // can't resolve the socket path; trust the env var
  }
  return std::filesystem::exists(std::filesystem::path(xdg) / sock, ec);
}

std::string ResolveKeyForConfig(const backend::BackendRegistry& reg,
                                const Configuration::Config& config) {
  const auto keys = reg.Keys();
  std::string hint = config.view.backend.value_or("");

  // An exact registry key (the bundle's [view] backend, or --backend) selects
  // that backend directly -- the primary runtime-selection path.
  if (!hint.empty() && reg.Resolve(hint) != nullptr) {
    return hint;
  }

  const auto has = [&](std::string_view k) -> const std::string* {
    for (const auto& key : keys) {
      if (key == k) {
        return &key;
      }
    }
    return nullptr;
  };
  const std::string* lease_egl = has("wayland-leased-drm-egl");
  const std::string* lease_vk = has("wayland-leased-drm-vulkan");
  const std::string* lease_sw = has("wayland-leased-drm-software");

  // The leased family is resolved BEFORE any fallback below, including the
  // single-backend short-circuit. Both fall back to a compiled-in backend when
  // the requested one is absent, which for a leased request is the one outcome
  // that must never happen: an unleased drm-kms-* opens a card and takes DRM
  // master outright. That is not a degraded version of leasing, it is the
  // opposite of it, on hardware the operator may not own. Order is load-bearing
  // here, not stylistic -- this guard sitting after the keys.size() == 1 check
  // is what let a leased request silently become drm-kms-egl.
  //
  // "wayland-leased-drm" as a bare family name resolves within the family:
  // vulkan -> egl -> software among the tiers actually registered. Vulkan leads
  // because a leased connector is a dedicated scanout target, which is what the
  // vulkan tier's explicit-ownership model is built for; egl is the fallback
  // for stacks without an ICD. This does NOT consult the legacy egl/vulkan
  // renderer hint -- that hint and this family name arrive in the same string,
  // so it can only ever be one or the other. Say wayland-leased-drm-egl to pin
  // egl.
  //
  // Anything else under the family prefix is an error rather than a fallback.
  // Leasing is not a preference, it is the difference between driving a
  // connector the compositor handed us and grabbing a card outright: quietly
  // running an unleased drm-kms-egl for someone who asked for a leased backend
  // would do the opposite of what they configured, on hardware they may not
  // own.
  constexpr std::string_view kLeasedFamily = "wayland-leased-drm";
  if (hint.rfind(kLeasedFamily, 0) == 0) {
    if (hint == kLeasedFamily) {
      if (lease_vk != nullptr) {
        return *lease_vk;
      }
      if (lease_egl != nullptr) {
        return *lease_egl;
      }
      if (lease_sw != nullptr) {
        return *lease_sw;
      }
    }
    ihs::log::error(
        "[backend] '{}' is not available in this build. Leased backends are "
        "not interchangeable with the direct ones, so this will not fall back "
        "to an unleased backend. Rebuild with "
        "BUILD_BACKEND_WAYLAND_LEASED_DRM=ON plus a renderer stack "
        "(BUILD_BACKEND_DRM_KMS_EGL / BUILD_BACKEND_DRM_KMS_VULKAN / "
        "BUILD_BACKEND_SOFTWARE+BUILD_SOFTWARE_SINK_DRM).",
        hint);
    return std::string{};
  }

  if (keys.size() == 1) {
    // Only one backend is compiled in, so it is the only choice. Warn if the
    // operator explicitly asked for a different one (a full key, not the legacy
    // egl/vulkan renderer hint) so a typo or wrong-build isn't silent. A leased
    // request never reaches here -- the guard above already refused it.
    if (!hint.empty() && hint != "egl" && hint != "vulkan" &&
        hint != keys.front()) {
      ihs::log::warn(
          "[backend] requested backend '{}' is not compiled into this build; "
          "using the only available backend '{}'",
          hint, keys.front());
    }
    return keys.front();
  }

  // Several backends and no exact key. Pick by environment, honoring the legacy
  // egl/vulkan renderer-family hint within the chosen family:
  //   - a live Wayland session -> a wayland-* backend (we run as a client);
  //   - otherwise              -> KMS-direct (drm-kms-* / software).
  const bool want_vulkan = (hint == "vulkan");
  const std::string* wl_egl = has("wayland-egl");
  const std::string* wl_vk = has("wayland-vulkan");
  const std::string* drm_egl = has("drm-kms-egl");
  const std::string* drm_vk = has("drm-kms-vulkan");
  const std::string* sw = has("software");

  if (WaylandSessionAvailable() && (wl_egl != nullptr || wl_vk != nullptr)) {
    if (want_vulkan && wl_vk != nullptr) {
      return *wl_vk;
    }
    return wl_egl != nullptr ? *wl_egl : *wl_vk;
  }

  // No (usable) Wayland session: prefer KMS-direct, then software.
  if (want_vulkan && drm_vk != nullptr) {
    return *drm_vk;
  }
  if (drm_egl != nullptr) {
    return *drm_egl;
  }
  if (drm_vk != nullptr) {
    return *drm_vk;
  }
  if (sw != nullptr) {
    return *sw;
  }
  // Only Wayland backends compiled but no session detected; return one anyway
  // (it will fail loudly at connect if truly headless).
  if (wl_egl != nullptr) {
    return *wl_egl;
  }
  if (wl_vk != nullptr) {
    return *wl_vk;
  }
  return std::string{};
}

bool EnsureActiveBackend(backend::BackendRegistry& registry,
                         const std::vector<Configuration::Config>& configs) {
  if (registry.HasActive()) {
    return true;
  }
  if (registry.Keys().empty()) {
    RegisterCompiledBackends(registry);
  }
  const std::string key = ResolveKeyForConfig(registry, configs[0]);
  const backend::BackendDescriptor* active = registry.Resolve(key);
  if (active == nullptr) {
    std::string available;
    for (const auto& k : registry.Keys()) {
      available += ' ';
      available += k;
    }
    ihs::log::critical(
        "[backend] could not resolve an active backend (key='{}'); "
        "registered:{}",
        key, available);
    return false;
  }

  // Report the resolved key when it is not what was configured -- a bare family
  // name, a legacy egl/vulkan hint, or an unset backend field all resolve to
  // something the operator never typed. The configured string is what gets
  // echoed as "Backend:", so without this there is no way to tell which tier a
  // family name actually landed on.
  if (const std::string configured = configs[0].view.backend.value_or("");
      key != configured) {
    ihs::log::info("[backend] resolved '{}' -> '{}'",
                   configured.empty() ? std::string{"<unset>"} : configured,
                   key);
  }

  // The "active" backend is the process-wide default: it backs the first
  // bundle and answers process-level queries (e.g. --drm-list-modes). Each view
  // independently resolves its own backend via ResolveKeyForConfig when its
  // display and Backend are built, so a later bundle naming a different backend
  // is honored rather than coerced onto this one.
  registry.SetActive(active);
  return true;
}
