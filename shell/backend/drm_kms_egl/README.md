# drm_kms_egl backend

Direct DRM/KMS + GBM + EGL/GLES2 backend for ivi-homescreen. Runs the
Flutter shell on a bare Linux TTY without any compositor — the binary
talks to the kernel mode-setting driver directly via [drm-cxx][drm-cxx],
opens its DRM device through libseat (logind / seatd / built-in), and
schedules scanout either through the atomic plane allocator or a legacy
`drmModeSetCrtc` + page-flip fallback. EGL/GLES2 renders Flutter layers
into gbm_bo's that the kernel scans out directly.

This document is the architecture + operations guide for the backend.

---

## Architecture

```
                 ┌─────────────────────────────────────────┐
                 │             FlutterEngine               │
                 └──┬──────────────────┬──────────────────┬┘
                    │ rasterizer       │ platform         │ ui
                    ▼                  ▼                  ▼
  ┌─────────────────────────┐   ┌──────────────────┐
  │       DrmBackend        │   │  embedder API    │
  │  drm::Device  + GBM     │   └──────────────────┘
  │  EGL/GLES2 contexts     │
  │  DriverProbe::Resolved  │   ┌──────────────────────────────┐
  │  ┌─────────┐ ┌────────┐ │   │       DrmDisplay             │
  │  │ Drm     │ │ Drm    │ │   │  ┌────────────────────────┐  │
  │  │Compositor│ │Cursor │ │   │  │     DrmSession         │  │
  │  └────┬────┘ └────────┘ │   │  │ drm::session::Seat     │  │
  │       │                 │   │  │ HotplugMonitor (udev)  │  │
  │  ┌────▼─────┐           │   │  │ libseat dispatch loop  │  │
  │  │DrmCapture│  optional │   │  └───────────┬────────────┘  │
  │  │ SIGUSR1  │           │   │              │               │
  │  └──────────┘           │   │  ┌───────────▼────────────┐  │
  │                         │   │  │      DrmSeat           │  │
  │                         │   │  │ libinput + xkbcommon   │  │
  │                         │   │  │ KeyRepeater            │  │
  │                         │   │  │ optional DrmCursor*    │  │
  └─────────────────────────┘   │  └────────────────────────┘  │
              ▲                 └──────────────────────────────┘
              │  drm_dev_->fd()
              ▼
        /dev/dri/cardN (libseat-revocable fd)
```

### Module responsibilities

- **`DrmSession`** (`drm_session.{h,cc}`) — process-wide libseat session.
  Opens `/dev/dri/cardN` and `/dev/input/event*` through the seat
  provider so the fds are master-capable and revocable on VT switch.
  Owns the dispatch thread that pumps `Seat::dispatch()` and the udev
  `HotplugMonitor`. Open() returns nullptr when no seat backend exists
  (no logind/seatd/builtin); callers fall back to a direct-open path
  with foreground-VT and `drmSetMaster` guards.

- **`DrmBackend`** (`drm_backend.{h,cc}`) — owns the DRM device, the
  GBM device + surface, the EGL display and contexts, the saved CRTC
  state for restoration on exit, and the compositor/cursor/capture
  objects. `Create()` runs in this order: `InitDrm` → `DriverProbe::
  Resolve` → `InitGbm` → `InitEgl` → instantiate `DrmCompositor`
  (when `BUILD_COMPOSITOR=ON`) → instantiate optional `DrmCursor` +
  `DrmCapture`.

- **`DrmCompositor`** (`drm_compositor.{h,cc}`, gated by
  `BUILD_COMPOSITOR=ON`) — Flutter compositor implementation. Two
  presentation paths:
  - **Plane allocator** (preferred): each Flutter layer gets its own
    overlay plane via `drm::planes::Allocator`. First atomic commit
    attaches `MODE_ID` / `ACTIVE` / connector `CRTC_ID` to bring the
    pipe up; subsequent commits are page-flips. Explicit-sync
    (`IN_FENCE_FD` / `OUT_FENCE_PTR`) is not wired today — implicit
    sync via dma-buf reservations covers every current producer.
  - **GL fallback** (`PresentViaGlFallback`): composes everything
    through a single GL framebuffer that `DrmBackend::Present` then
    drives via legacy `drmModeSetCrtc` + page-flip.
    Latches via `fallback_latched_` on a real allocator/commit
    failure; the next frame skips the plane path. EACCES (master
    loss during libseat pause race) is short-circuited and does
    NOT latch — see drm_compositor.cc's `errc::permission_denied`
    handling.

- **`DriverProbe::Resolved`** (`driver_probe.{h,cc}`) — runtime config.
  Every `kAuto` field in `DrmConfig` is resolved here against driver
  caps + plane introspection. Holds parsed EDID `ConnectorInfo`
  (monitor name + HDR static metadata) when the connector exposes one.
  `LogResolved` is the one-line startup summary visible in journal
  output.

- **`DrmCursor`** (`drm_cursor.{h,cc}`, gated by libxcursor →
  `HAVE_DRM_CURSOR`) — KMS hardware cursor on a CURSOR-type plane
  (or legacy `drmModeSetCursor` fallback). Loads the system "default"
  arrow at DPI-scaled size from the XDG icon theme. Pointer position
  is plumbed in from `DrmSeat::HandlePointerMotion` via an atomic
  pointer. The first atomic commit is deferred to the first pointer
  motion so it doesn't race the compositor's first-commit modeset.

- **`DrmCapture`** (`drm_capture.{h,cc}`, gated by blend2d →
  `HAVE_DRM_CAPTURE`) — SIGUSR1-driven CRTC snapshot diagnostic. See
  [Capturing a snapshot](#capturing-a-snapshot) below.

- **`DrmDisplay`** (`shell/display/drm_display.{h,cc}`) — non-backend
  container. Owns the `DrmSession` (so its lifetime spans backend
  re-creation) and `DrmSeat`. Exposes `SetCursor(DrmCursor*)` to wire
  the backend-owned cursor to the seat dispatch thread after backend
  creation.

- **`DrmSeat`** (`shell/input/drm_seat.{h,cc}`) — libinput-backed seat
  on its own dispatch thread. Translates evdev events to
  `FlutterPointerEvent` / `FlutterKeyEvent`, drives auto-repeat via
  `drm::input::KeyRepeater`, forwards pointer motion to the optional
  `DrmCursor`. Exposes a dormant `ReloadKeymap(KeymapConfig)` API
  for a future settings/locale channel — last-writer-wins via a
  thread-safe pending slot.

### Threading model

- **Main thread**: argv parse, Flutter engine bring-up, the run loop
  that pumps the engine.
- **Flutter rasterizer thread**: calls into `DrmBackend::Present` (GL
  fallback) or `DrmCompositor::PresentLayers` (plane path). All
  KMS commits originate here.
- **DrmSession dispatch thread**: services libseat events
  (pause/resume) and the udev `HotplugMonitor`. Long-running but
  mostly idle.
- **DrmSeat dispatch thread**: drives libinput on
  `seat_->poll_fd()`, drains the `KeyRepeater` timerfd, posts
  `FlutterPointerEvent` / `FlutterKeyEvent` onto the platform task
  runner.

Cursor commits (when the cursor is active) race the compositor's
commits at the libdrm boundary; the kernel serializes
`drmModeAtomicCommit` ioctls per fd. drm::cursor::Renderer is single-
thread-owned (the seat thread); the compositor never touches it.

---

## Features

| Capability | Status | Toggle / scope |
|---|---|---|
| Atomic plane allocator (overlay-per-layer) | ✓ | `BUILD_COMPOSITOR=ON` + driver supports atomic |
| Legacy `drmModeSetCrtc` + GL fallback | ✓ | Always (driver auto-detect + commit-failure latch) |
| Foreground-VT guard (no-libseat path) | ✓ | Refuses to run from a non-foreground VT |
| libseat session (logind/seatd/builtin) | ✓ | Auto-detected at startup |
| Connector ranking + `--drm-connector` pin | ✓ | Internal panels (eDP/LVDS/DSI/DPI) preferred |
| EDID + HDR static-metadata logging | ✓ | Logged at startup via libdisplay-info |
| Auto key-repeat | ✓ | `IVI_DRM_KEY_REPEAT=0` to disable |
| Dormant `ReloadKeymap` API | ✓ | No caller wired; intended for settings channel |
| KMS HW cursor (CURSOR plane or legacy) | ✓ | Requires libxcursor; `IVI_DRM_CURSOR=0` to disable |
| Connector hotplug observability | ✓ | Logs udev events; no auto-remodeset yet |
| SIGUSR1 CRTC PNG snapshot | ✓ | Requires Blend2D; `IVI_DRM_CAPTURE=1` to arm |
| EACCES (master loss) skip-frame | ✓ | Always; avoids latching GL fallback during libseat pause |
| HDR signaling (HDR_OUTPUT_METADATA) | deferred | Waiting on Flutter HDR API |
| Explicit sync (`IN_FENCE_FD` / `OUT_FENCE_PTR`) | deferred | No per-frame producer needs it yet |

---

## Build steps

### Dependencies

Ubuntu 24.04 / Debian 13 / Fedora 41+:

```bash
sudo apt-get install -y \
  ninja-build cmake pkg-config meson \
  libwayland-dev wayland-protocols libxkbcommon-dev \
  libegl1-mesa-dev libgles2-mesa-dev mesa-common-dev \
  libdrm-dev libgbm-dev libinput-dev libudev-dev \
  libxcursor-dev \
  libseat-dev \
  libglib2.0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
```

Optional (enables `drm::capture` / SIGUSR1 snapshot):

```bash
sudo apt-get install -y libblend2d-dev
```

> `libdisplay-info ≥ 0.2.0` is required by the EDID readout. Ubuntu
> 24.04 ships 0.1.1; the CI workflow at `.github/workflows/drm-kms.yml`
> auto-builds 0.2.0 from source via the
> `.github/actions/setup-libdisplay-info` composite action when the host
> is too old. Local devs on 24.04 need the same fallback or a backport.

### Configure + build

DRM-only build (the canonical configuration for this backend):

```bash
cmake -GNinja -B cmake-build-debug-clang \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_BACKEND_DRM_KMS_EGL=ON \
  -DBUILD_BACKEND_WAYLAND_EGL=OFF \
  -DBUILD_BACKEND_WAYLAND_VULKAN=OFF \
  -DBUILD_COMPOSITOR=ON

ninja -C cmake-build-debug-clang
```

`BUILD_BACKEND_DRM_KMS_EGL` is mutually exclusive with the two Wayland
backends — disabling them is mandatory.

`BUILD_COMPOSITOR=ON` is recommended; it enables the atomic plane
allocator path. Without it, the build still works but only the legacy
GL fallback runs and `drm_compositor.cc` is excluded.

Output binary: `cmake-build-debug-clang/shell/homescreen` (the name is
`EXE_OUTPUT_NAME` from `cmake/options.cmake:204` — default "homescreen").

### Optional features detected at configure time

- **libxcursor** missing → `HAVE_DRM_CURSOR` undefined, `drm_cursor.cc`
  not compiled, no HW cursor available at runtime
- **blend2d** missing → `HAVE_DRM_CAPTURE` undefined, `drm_capture.cc`
  not compiled, SIGUSR1 snapshot becomes a no-op
- **libseat** missing → `DrmSession::Open()` returns nullptr, backend
  falls back to direct `/dev/dri/card*` open + foreground-VT guard

All three are configure-time soft probes; missing any one is logged at
`message(STATUS …)` and the build continues.

---

## Running

The shell needs a Flutter app bundle. Canonical bundle for smoke
testing (per the project's memory notes): `tcna-packages/
video_player_linux/example/player/.desktop-homescreen/`.

```bash
# Switch to a bare TTY (Ctrl+Alt+F3 or similar) so something else
# isn't holding DRM master; or run via systemd with seatd/logind.
./cmake-build-debug-clang/shell/homescreen \
  -b /path/to/.desktop-homescreen/ \
  --drm-device=/dev/dri/card1
```

Typical first-run log lines worth reading:

```
[DrmSession] libseat session via logind acquired
[DrmBackend] opened /dev/dri/card1 via libseat (fd=N)
[DrmBackend] picked connector eDP-1 via rank
[DrmBackend] connector=N crtc=M mode=1920x1080@60Hz
[DrmBackend] driver='amdgpu' compositor=planes modeset=atomic
              primary-fmt=XR24 bs-fmt=AR24 overlay-planes=yes ...
[DrmBackend] panel='Samsung TBD' hdr=type1:y sdr:y hdr:y pq:y hlg:n
              lum-max=1000 lum-min=0.0050
[DrmCursor] ready (sprite=24px buffer=64px path=atomic-cursor plane_id=N)
```

### Useful CLI flags

| Flag | What it does |
|------|-------------|
| `-b <path>` | Path to the Flutter app bundle (required) |
| `--drm-device=<path>` | Override `/dev/dri/card1` |
| `--drm-connector=<name>` | Pin a specific connector (e.g. `eDP-1`, `HDMI-A-1`) |
| `--drm-list-modes[=<dev>]` | Print every connector + its modes, then exit |
| `--drm-compositor=auto\|planes\|gl` | Force compositor strategy |
| `--drm-modeset=auto\|legacy\|atomic` | Force modeset API |
| `--drm-allow-nonblock-modeset=auto\|yes\|no` | Override `NONBLOCK \| ALLOW_MODESET` quirk |
| `--drm-primary-format=auto\|xrgb8888\|xbgr8888\|argb8888\|abgr8888\|rgb565` | Force primary plane format |
| `--drm-overlay-planes=auto\|yes\|no` | Disable overlay-plane scanout |
| `--drm-explicit-sync=auto\|yes\|no` | Reserved knob; not consumed today (no per-frame fence producer) |
| `--drm-async-flip=auto\|yes\|no` | DRM_MODE_PAGE_FLIP_ASYNC for tearing updates |
| `-f` / `--fullscreen` | Drive panel at preferred mode (clears explicit w/h) |
| `--debug-backend` | Verbose per-frame plane assignment log |

Every `--drm-*` flag has a `HOMESCREEN_DRM_*` env-var equivalent and a
`view.drm_*` TOML key. CLI > env > TOML.

### Env vars for runtime toggles

| Env | Default | Effect |
|------|---------|--------|
| `IVI_DRM_HOTPLUG` | (on) | `0` disables the udev hotplug monitor |
| `IVI_DRM_KEY_REPEAT` | (on) | `0` disables auto-repeat for held keys |
| `IVI_DRM_CURSOR` | (on) | `0` disables the KMS HW cursor |
| `IVI_DRM_CAPTURE` | (off) | `1` arms the SIGUSR1 snapshot handler |
| `VIDEO_PLAYER_AUDIO_SINK` | — | Set to `alsasink` on bare TTY (no PipeWire) |

---

## Capturing a snapshot

The SIGUSR1 snapshot diagnostic dumps the current per-plane composition
of the active CRTC to a PNG. Use it for "why did the screen render
wrong" tickets — captures actual scanout pixels, not just commit state.

### Prerequisites

- Build configured with Blend2D present (`message(STATUS) blend2d
  found` at cmake time, or check the link line for `libblend2d.so`).
- `IVI_DRM_CAPTURE=1` in the binary's environment at startup.

### Procedure

Start the binary with the env var set:

```bash
IVI_DRM_CAPTURE=1 ./cmake-build-debug-clang/shell/homescreen \
  -b /path/to/bundle/ &
```

You'll see this in the log on a successful arm:

```
[DrmCapture] SIGUSR1 capture armed; kill -USR1 12345 drops <dir>/homescreen-snapshot-<ms>.png
```

From a second terminal (or SSH), trigger a capture:

```bash
# By PID (the startup log printed it; or use pgrep)
kill -USR1 $(pgrep -x homescreen)

# Or by name directly
pkill -USR1 -x homescreen
```

Each signal produces one PNG. The frame on which the signal is
observed stutters by tens of milliseconds (DMA-BUF readback + PNG
encode); the next frame returns to normal.

### Output location

- **Preferred**: `$XDG_RUNTIME_DIR/homescreen/homescreen-snapshot-<unix-ms>.png`
  (mode-0700 per-user dir owned by systemd-logind).
- **Fallback** (no `XDG_RUNTIME_DIR`): `/tmp/homescreen-snapshot-<unix-ms>.png`,
  with `lstat()`-refusal if the file already exists (symlink-trap
  protection — a same-UID attacker can't pre-plant a symlink to
  redirect the write).

The directory + filename prefix come from CMake's `EXE_OUTPUT_NAME` via
`kApplicationName` in `config/common.h`; a rebadged build picks up its
own name.

### Limitations

- Tiled / AFBC / DCC / compressed modifiers are skipped with a warning
  (V1 of drm-cxx's snapshot).
- YUV-format planes (typical video overlays) are skipped with a warning
  — covering them needs GPU-assisted readback.
- Each capture is bounded by free space in the output dir.

---

## Diagnostics

### `--drm-list-modes`

Lists every connector and its modes without bringing up the engine.
Run from anywhere, no TTY required:

```bash
./homescreen --drm-list-modes=/dev/dri/card1
```

### `--debug-backend`

Per-frame plane assignment log (one line per layer):

```
[DrmCompositor] 3 of 5 layers on HW planes
[DrmCompositor]   layer 0 → plane 42 (fb_id=N, 1920x1080, zpos=0)
[DrmCompositor]   layer 1 → plane 43 (fb_id=N, 256x256, zpos=1)
[DrmCompositor]   layer 2 → composition (overflow)
[DrmCompositor]   comp_layer → plane 44 (fb_id=N)
```

### Hotplug events

Connector plug/unplug is logged at info level. The handler is
observability-only today — there is no auto-remodeset on disconnect.

```
[DrmBackend] hotplug: devnode=/dev/dri/card1 connector_id=N (active connector)
```

### Recovery from a SIGKILL'd bare-TTY run

The SIGKILL reverse-watchdog was removed in commit `2ab5004a` — when
libseat is active, seatd/logind cleans up the session on socket drop.
On the no-libseat path, a SIGKILL'd shell may leave the VT in K_OFF
(keystrokes don't echo) and the CRTC scanning out a freed BO. Recover
with:

```bash
sudo kbd_mode -a       # restore default kb mode on current VT
sudo chvt 7 && sudo chvt 1   # round-trip to force the kernel to reprogram CRTC
```

`stty sane` is insufficient — it touches the line discipline only, not
`KDSKBMODE`.

### Refused on a non-foreground VT (no-libseat path)

```
[DrmBackend] controlling terminal is not a kernel VT (major=136, minor=N) —
    you're running from a terminal emulator or SSH. Active VT is ttyN.
    Switch to ttyN (Ctrl+Alt+FN) and rerun ...
```

This guard exists because `drmSetMaster` can succeed from a non-
foreground VT but scanout still goes to whoever owns the foreground —
atomic commits silently accept, no PAGE_FLIP_EVENT fires, the shell
hangs waiting for the next flip. Fail-fast is better.

---

## File map

```
shell/backend/drm_kms_egl/
  drm_backend.{h,cc}      DRM device + GBM + EGL; saved CRTC
  drm_compositor.{h,cc}   Plane allocator + GL fallback (BUILD_COMPOSITOR)
  drm_cursor.{h,cc}       KMS HW cursor (HAVE_DRM_CURSOR)
  drm_capture.{h,cc}      SIGUSR1 snapshot diagnostic (HAVE_DRM_CAPTURE)
  drm_session.{h,cc}      libseat session + udev hotplug
  driver_probe.{h,cc}     Auto-knob resolution + EDID readout
  README.md               this file

shell/display/drm_display.{h,cc}  DrmDisplay (owns DrmSession + DrmSeat)
shell/input/drm_seat.{h,cc}       libinput + xkbcommon + KeyRepeater

third_party/drm-cxx/              vendored drm-cxx submodule
.github/workflows/drm-kms.yml     CI workflow (gcc + clang + vkms-smoke)
.github/actions/setup-libdisplay-info/  build libdisplay-info ≥0.2.0
```

---

## References

- [drm-cxx][drm-cxx] — vendored at `third_party/drm-cxx`; the bulk of
  the KMS API ergonomics live there.
- [DRM kernel docs][drm-kernel] — `IOCTL_MODE_ATOMIC`, plane types,
  property semantics.
- [systemd-logind seat API][logind-seat] — the canonical libseat
  backend on most distros.

[drm-cxx]: https://github.com/jwinarske/drm-cxx
[drm-kernel]: https://docs.kernel.org/gpu/drm-kms.html
[logind-seat]: https://www.freedesktop.org/software/systemd/man/sd-login.html
