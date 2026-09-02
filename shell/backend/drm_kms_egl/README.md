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
  `HotplugMonitor`. The dispatch loop blocks indefinitely on a
  `WakeEventFd` (reusing `shell/input/wake_event_fd.h`) rather than
  polling every 200 ms — both watched fds (libseat fd, hotplug fd) are
  readable only when there is something to dispatch, so the old timeout
  existed solely to re-evaluate the `stop_` flag, which changes exactly
  once at teardown. The eventfd is written once in the destructor after
  `stop_` is set, closing the lost-wake window across the check/block
  boundary. When the eventfd is unavailable (`fd() == -1`) the loop
  degrades to a 200 ms cap as fallback.
  Open() returns nullptr when no seat backend exists
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

### Build matrix (compositor / scene)

Two CMake options select the present path. They are independent of the
runtime `--drm-compositor auto|planes|gl` flag (which only picks among
what was compiled in):

| Config | CMake flags | Present path compiled in |
|--------|-------------|--------------------------|
| **C1** | `-DBUILD_COMPOSITOR=OFF` | Legacy GL only — `eglSwapBuffers` + `drmModePageFlip`. `drm_compositor.cc` is excluded. |
| **C2** | `-DBUILD_COMPOSITOR=ON` | Full path: `LayerScene` direct-scanout when the primary supports `REFLECT_Y`, GL composite otherwise. **Recommended / canonical.** |

`BUILD_COMPOSITOR=ON` compiles `DrmCompositor` together with the drm-cxx
`LayerScene` present path; the GL composite runs as a runtime fallback
where no plane supports `REFLECT_Y` (or a frame overflows the backing
store / carries a platform view). The build matrix exercises both C1 and
C2 on x86_64 and aarch64.

The aarch64 cross-build maps these as: emb's `drm-kms-egl` backend → **C1**
(default direct DRM/KMS + GBM); adding `BUILD_COMPOSITOR=ON` to that
backend's `defines` → **C2**.

Per-driver note: on **rockchip** (rk3588 VOP2) the primary advertises
`REFLECT_Y` and passes a `TEST_ONLY` atomic commit with it, but a live
reflected scanout faults the display IOMMU; C2 therefore force-selects
the GL-composite sub-path on that driver (zero-copy direct-scanout is
not usable there). vc4 and amdgpu are unaffected.

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
| `--drm-mode=<W>x<H>@<R>` | Pin a specific mode (e.g. `1920x1080@120`); default = preferred mode |
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
| `IVI_DRM_VSYNC` | (on) | `0` falls back from Flutter `vsync_callback` (PAGE_FLIP_EVENT-locked) to the engine's internal wall-clock scheduler |
| `IVI_DRM_RT` | (off) | Set to anything non-empty to enable per-thread priority elevation via Flutter's `thread_priority_setter` — rasterizer gets `SCHED_FIFO` prio 2, UI thread `SCHED_FIFO` prio 1, background tasks `SCHED_BATCH`. The deliberately low prios let amdgpu / ksoftirqd kthreads preempt the rasterizer during `glFinish` (bumping to prio 10 regressed cadence from 98% → 41% on this hardware). The platform task runner thread (asio flip monitor) is covered too. DrmSession / DrmSeat stay at default. |
| `IVI_DRM_FLIP_TRACE` | (off) | `1` logs every PAGE_FLIP_EVENT (frame-cadence diagnostic) |
| `IVI_DRM_PROFILE` | (off) | Anything non-empty enables per-frame composite profiling. Every 60 frames, `PresentFramed` / `PresentLayers` log a line: `framed/planes profile (n=60): wait=Xms compose=Yms commit=Zms total=Wms` with both per-stage mean and max. Useful for diagnosing where the per-frame budget goes. |
| `IVI_DRM_NO_DIRECT_SCANOUT` | (off) | `1` forces GL composite even when REFLECT_Y is available. Diagnostic — bisects visual artifacts that may live on the direct-scanout path. |
| `VIDEO_PLAYER_AUDIO_SINK` | — | Set to `alsasink` on bare TTY (no PipeWire) |

#### Real-time scheduling: capability setup

`IVI_DRM_RT` calls `pthread_setschedparam(SCHED_FIFO, ...)` on the rasterizer + UI threads (from inside Flutter's `thread_priority_setter` callback). The kernel rejects this with `EPERM` unless the process holds `CAP_SYS_NICE` (or runs as root). For an unprivileged install, grant the capability once on the binary:

```bash
sudo setcap cap_sys_nice=eip cmake-build-debug-clang/shell/homescreen
```

Then any user can run it with `IVI_DRM_RT=1`. Without the capability the `pthread_setschedparam` call silently returns `EPERM` and the thread stays at `SCHED_OTHER` — no warning, no partial fallback. If your `IVI_DRM_RT` flag isn't visibly helping, check that `getcap <binary>` reports `cap_sys_nice=eip`.

Default off because a `SCHED_FIFO` thread that spins (infinite loop, deadlock) is unrecoverable without a hard reset — that's painful during compositor development. Once the code earns trust the default should flip on.

On an idle system the boost is small (~+10pp of frames hitting the vblank deadline in local benchmarks at 60 Hz). It pays off proportionally more at higher refresh rates (240 Hz has a 4.2 ms vblank budget — much tighter) and on systems with competing CPU load.

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

## Benchmarks

Measured on amdgpu (RDNA/Polaris-class), `feat/drm-kms-egl @ 85a78d10`, Fedora 43 kernel 6.19, running the flutter-wonderous-app bundle.

### Methodology

`IVI_DRM_FLIP_TRACE=1` logs every PAGE_FLIP_EVENT. The intervals between consecutive flips give the achieved cadence — that's the metric here. Each log is parsed with an awk one-liner that buckets intervals into native vblank multiples (e.g. at 240 Hz: ≤6 ms = 240 Hz hit, 7–11 ms = 120 Hz miss, 12–19 ms = 60 Hz miss). The reproducer scripts live in `/tmp/run-rt-test*.sh` in dev environments.

For a per-stage breakdown, `IVI_DRM_PROFILE=1` adds a log line every 60 frames with mean+max of `wait`/`compose`/`commit`/`total` from `PresentFramed` (framed mode) or `PresentLayers` (plane allocator) — see the env table above.

### Headline result

**240 Hz panel (2560×1440), wonderous fullscreen, `IVI_DRM_RT=1` + `IVI_DRM_VSYNC=1`:**

| Cadence bucket | Frames | % |
|---|---:|---:|
| 240 Hz native (≤6 ms) | 63 479 | **98.0%** |
| 120 Hz (7–11 ms) | 1 141 | 1.8% |
| 60 Hz (12–19 ms) | 134 | 0.2% |
| 30 Hz (20–36 ms) | 5 | ~0% |
| Idle/pause | 9 | ~0% |

Avg interval 4.31 ms, p50 4 ms, p95 4 ms, p99 8 ms — a 64 769-frame sample.

### Path that landed the improvements

The 240 Hz vblank budget is 4.17 ms, so even small scheduling jitter or per-frame overhead is fatal. Pre-optimization the same setup achieved 23.9 % native at 240 Hz; the cumulative effect of these commits walked it to 98 %:

1. **asio-driven PAGE_FLIP_EVENT monitor** (`6bab1163`). Drains the drm fd on the platform task runner instead of waiting for the rasterizer to poll inside `WaitForPendingFlip`. Decouples baton return from Present cadence and is the prerequisite for the next commit.
2. **Wired `FlutterVsyncCallback`** (`226a9158`). Lets Flutter lock its frame pacer to actual vblank instead of its internal wall-clock scheduler. `IVI_DRM_VSYNC=0` reverts to wall-clock for bisection.
3. **Per-thread RT priority via `thread_priority_setter`** (`5ae8ae50`). Flutter's rasterizer gets `SCHED_FIFO` prio 2; UI thread gets `SCHED_FIFO` prio 1; background pool gets `SCHED_BATCH`. Gated by `IVI_DRM_RT` because a runaway RT thread is unrecoverable without a hard reset. Eliminating raster-thread preemption jitter is the single biggest contributor at high refresh rates. **Counter-intuitive:** prio 2 outperforms prio 10 by ~57 percentage points at 240Hz — at higher prios the rasterizer can starve the very amdgpu / ksoftirqd kthreads that signal the GPU fences `glFinish` is waiting on.
4. **Composite-path gating + direct-scanout fast path** (`85a78d10`). `PresentLayers` skips its GL composite block when no Flutter layer needs composition, and tries direct-scanout (set `PixelFormat` + `FbModifier` + `REFLECT_Y` rotation on the layer) when the primary plane's rotation bitmask allows it. On hardware that supports `REFLECT_Y` the per-frame compositor cost drops from ~7 ms → ~0.16 ms (43×); on hardware that doesn't, the path falls back cleanly to the prior composite cost.
5. **Composite profiling** (`bda9c638`). The instrumentation that made the cost breakdown legible.

### Where the remaining 2% comes from

The 1.8 % at 120 Hz buckets are individual frames missing the next vblank by a few hundred microseconds. Per-stage profiling on the same workload shows `compose` averaging ~5 ms with occasional max excursions of 7–15 ms (Flutter render spikes from text layout or texture upload). Eliminating those would need work on the Flutter side, not the compositor. The compositor itself is verified efficient: `commit` averages 0.02–0.06 ms.

### Direct-scanout (when REFLECT_Y is available)

On hardware with at least one CRTC plane supporting REFLECT_Y (modern Intel / Mali / newer amdgpu generations), `PresentLayers` switches to direct-scanout. Single-fullscreen-BS frames bypass GL composition entirely:

| Path | wait | compose | commit | total |
|---|---:|---:|---:|---:|
| Composite (REFLECT_Y unavailable) | 1.69 ms | 5.26 ms | 0.06 ms | 7.00 ms |
| Direct-scanout (REFLECT_Y available) | 0.00 ms | 0.15 ms | 0.01 ms | **0.16 ms** |

The 43× compositor speedup expands the per-frame headroom at any refresh rate. On a 240 Hz panel where the budget is 4.17 ms, going from "7 ms compositor + Flutter render" to "0.16 ms compositor + Flutter render" turns "always missing one vblank" into "always making vblank."

The probe happens once in `InitPlaneAllocator`; the log line is `[DrmCompositor] direct-scanout REFLECT_Y available: yes|no`. Operators can read it once at startup to know which path their hardware will take.

#### Multi-buffered BS + explicit sync (how direct-scanout works on every driver)

Direct-scanout binds Flutter's backing-store gbm_bo directly to the primary plane (no intermediate composite BO). Two races have to be eliminated for this to be visually correct on every driver:

1. **BS recycle race.** Flutter wants to start frame N+1 immediately after PresentLayers returns; if the BS is single-buffered, Flutter's writes trample the BO that the kernel is still scanning out. amdgpu / Intel mask this via implicit dma-fence sync — the kernel holds the BO during scanout and the GPU stalls Flutter's next write. `nvidia-drm` (validated on Jetson Orin L4T R36.4.7) does **not** insert that fence; the race produces visible whole-frame flicker.
2. **GPU-write completion race.** Even with multi-buffering, the kernel can scan out a BO before Flutter's GPU writes to it have actually landed in memory. amdgpu / Intel implicit-sync also covers this. `nvidia-drm` doesn't.

The backend solves both with a **3-slot BS pool + IN_FENCE_FD**:

- Each Flutter BS owns a pool of 3 `gbm_bo`s. `PresentLayers` rotates the FBO's color attachment between slots after every commit — Flutter's frame N+1 renders into a different BO from the one the kernel is scanning out. Slot release is event-driven via `PAGE_FLIP_EVENT`, so any pool size ≥ 2 is correctness-safe; default 3 gives one slot in flight + one queued + one ready to render.
- Before the atomic commit, the backend creates an `EGL_SYNC_NATIVE_FENCE_ANDROID` fence and attaches its dup'd FD to each direct-scanout layer as the plane's `IN_FENCE_FD` property. The kernel waits on the fence (asynchronously, kernel-side) before scanning out the BO. Flutter's writes are guaranteed visible at scanout time without a synchronous user-space stall.
- When the EGL native-fence-sync extension is missing, the path falls back to a synchronous `glFinish` before commit (correctness preserved, ~2 ms perf cost).

Pool size auto-scales: BSes get 3 slots only when `any_plane_supports_reflect_y_` is true (i.e. direct-scanout is reachable). Drivers without REFLECT_Y or with the universal kill-switch `IVI_DRM_NO_DIRECT_SCANOUT=1` keep pool_size=1 to avoid 3× GBM allocation footprint on memory-constrained targets.

Validated end-to-end on `nvidia-drm` Jetson Orin L4T R36.4.7: clean 60 Hz lock at 1440p with the full direct-scanout speedup, no visible flicker. Composite path remains the safe fallback via `IVI_DRM_NO_DIRECT_SCANOUT=1` if a new driver release ever exhibits a regression.

### Tegra L4T (nvidia-drm) — measured numbers

Measured on Jetson Orin (`nvidia-drm`), L4T R36.4.7 / kernel 5.15.148-tegra, LG UltraGear+ on DP-1, `flutter/examples/image_list`, `IVI_DRM_RT=1 IVI_DRM_VSYNC=1 IVI_DRM_PROFILE=1`. Default config — multi-buffered BS pool (3 slots) + IN_FENCE_FD active. Steady-state means computed from the last 4 profile windows (= 240 frames) of each run; numbers reproduced across two independent runs at each refresh rate.

**2560×1440 @ 60 Hz** (panel preferred mode, vblank budget 16.67 ms):

| Stage | mean (steady) | max | vs GL composite |
|---|---:|---:|---:|
| wait | 9.17 ms | 9.65 ms | ~equal |
| compose | 0.79 ms | 1.02 ms | -2.76 ms |
| commit | 0.63 ms | 0.97 ms | -0.16 ms |
| **total** | **10.59 ms** | 11.38 ms | **-3.69 ms** |

Clean 60 Hz lock with **6.08 ms vblank headroom**. Direct-scanout compose is ~0.8 ms (vs ~3.6 ms when the same workload routes through GL composite) — `IN_FENCE_FD` keeps the GPU-wait kernel-side rather than stalling our user-space thread.

**1920×1080 @ 120 Hz** (`--drm-mode=1920x1080@120`, vblank budget 8.33 ms):

| Stage | mean (steady) | max |
|---|---:|---:|
| wait | 2.46 ms | 3.93 ms |
| compose | 1.03 ms | 1.79 ms |
| commit | 0.71 ms | 1.27 ms |
| **total** | **4.20 ms** | 6.20 ms |

Clean 120 Hz lock with **4.13 ms vblank headroom**. Steady-state flip cadence measured at 8.33 ms ± 0.08 ms — the panel's vblank period to single-digit microsecond precision. GL composite alone (i.e. without direct-scanout) cannot make 120 Hz on this hardware: its compositor cost sits at ~3.4 ms baseline, which combined with Flutter render variance pushes individual frames past the 8.33 ms budget.

**Reproducibility:** the steady-state totals above replicate within ±5% across multiple independent runs at each refresh rate. Image-decode hitches during `image_list` startup produce one frame near the budget edge in each run; after Flutter's loading phase completes the cadence locks cleanly.

**Out of reach (not yet validated):** 240 Hz at any resolution; 120 Hz at 1440p (panel doesn't expose this mode on DP-1 — see `--drm-list-modes` output).

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
