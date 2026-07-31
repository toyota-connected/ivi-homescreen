# software backend

CPU rendering for the Flutter embedder. Wires `FlutterRendererType.kSoftware`
to a pluggable `ISurfaceSink`. No GPU, no Wayland, no Mesa runtime in
the build matrix.

Two intended use cases:

1. **CI** — run any Flutter bundle without a display server and without
   GPU passthrough into the container. The engine boots, the bundle's
   Dart code runs, layouts compose, frames either drop on the floor or
   land on disk as PAM goldens.
2. **GPU-less devices** — minimal embedded hardware that ships a panel
   driven by `/dev/fb*` (fbdev) or a DRM dumb buffer (modern modesetting
   without a render-capable GPU). The CPU does both Flutter's raster
   work and the present-side memcpy / swizzle.

## Features

| Capability | Status | Toggle / scope |
|---|---|---|
| CPU `kSoftware` renderer → pluggable `ISurfaceSink` | ✓ | `-DBUILD_BACKEND_SOFTWARE=ON` |
| `NoneSink` — discards every frame (CI engine-only smoke) | ✓ | `IVI_SW_SINK=none` (default); always compiled in |
| `MemorySink` — mutex-guarded latest-frame snapshot for in-process fixtures | ✓ | `IVI_SW_SINK=memory`; always compiled in |
| `FileSink` — write each frame as a NetPBM PAM (P7) golden | ✓ | `IVI_SW_SINK=file:<pattern>`; always compiled in |
| `FbDevSink` — mmap `/dev/fb*`, auto-detect 32-bpp BGRA/BGRX or 16-bpp RGB565 | ✓ | `IVI_SW_SINK=fbdev[:<dev>]`; `BUILD_SOFTWARE_SINK_FBDEV` (ON) |
| `DrmDumbSink` — DRM dumb buffer, page-flip, kernel-timestamped vsync | ✓ | `IVI_SW_SINK=drm-dumb[:<dev>]`; `BUILD_SOFTWARE_SINK_DRM` (auto/libdrm) |
| Page-flip-locked Flutter vsync (drm-dumb only) | ✓ | `IVI_SW_VSYNC=0` for wall-clock fallback |
| RGB565 scanout format for drm-dumb | ✓ | `IVI_SW_DRM_FORMAT=rgb565` |
| Bayer 4×4 ordered dithering on RGB565 pack path | ✓ | `IVI_SW_DRM_DITHER=1` (drm-dumb + fbdev) |
| libinput `SoftwareSeat` — pointer / keyboard / key-repeat / multi-touch | ✓ | `IVI_SW_INPUT` (auto); `BUILD_SOFTWARE_INPUT_LIBINPUT` (auto) |
| Frame-count-bounded runtime for CI | ✓ | `IVI_SW_STOP_AFTER_FRAMES=<N>` |
| Present-call + vsync cadence profiling | ✓ | `IVI_SW_PROFILE` / `IVI_VSYNC_PROFILE` |
| Platform-view software compositor | ✗ deferred | `GetCompositorConfig()` returns all-null callbacks |
| fbdev vsync (`FBIO_WAITFORVSYNC`) | ✗ | Broadly broken/non-standard across drivers; use drm-dumb |

---

## Architecture

```mermaid
flowchart LR
    FE["Flutter Engine<br/>software.surface_present_callback(buf, rb, h)"]
    FE -->|kSoftware present| PT["SoftwareBackend::PresentTrampoline()"]
    PT -->|Present(buf, row_bytes, height)| SINK{{ISurfaceSink}}

    SINK --> NONE["NoneSink<br/>(drop)"]
    SINK --> MEM["MemorySink<br/>(memcpy → vector)"]
    SINK --> FILE["FileSink<br/>(write PAM)"]
    SINK --> FB["FbDevSink<br/>(mmap + swizzle)"]
    SINK --> DRM["DrmDumbSink<br/>(page-flip + vsync)"]

    DRM -->|PAGE_FLIP_EVENT| VS["vsync_callback<br/>(kernel scanout timestamp)"]
    VS -.->|FlutterEngineOnVsync| FE
```

`SoftwareBackend` is the Flutter-facing surface — it implements
`GetRenderConfig` with `type=kSoftware` plus a static present trampoline,
and owns the sink. `ISurfaceSink` is the abstraction: one
`Present(const void*, size_t row_bytes, size_t height)` plus optional
vsync hooks. Sinks are picked at startup from the `IVI_SW_SINK` env var.
Sinks are single-file and small: no threading, no buffer pooling. The
callback runs on Flutter's rasterizer thread; the sink either copies /
writes / mmaps synchronously and returns.

`SoftwareDisplay` is a no-op `IDisplay` so `App::Loop`'s sleep math has a
refresh-rate denominator. It defaults to 60 Hz; nothing else interesting
lives there. It also owns the libinput-backed `SoftwareSeat` (parallel to
how `DrmDisplay` owns `DrmSeat`).

### Module responsibilities

- **`SoftwareBackend`** ([software_backend.cc](software_backend.cc),
  [software_backend.h](software_backend.h)) — the Flutter-facing `Backend`.
  Implements `GetRenderConfig` (`type=kSoftware`) plus the static
  `PresentTrampoline`, owns the active sink, resolves the framebuffer extent
  from the sink's `NativeSize()`, gates `GetVsyncCallback()` on the sink
  advertising a real vblank source, and emits the `IVI_SW_PROFILE` session
  summary on clean dtor.
- **`ISurfaceSink`** ([surface_sink.h](surface_sink.h)) — the present
  abstraction: `Present(buf, row_bytes, height)` plus optional vsync /
  cursor / task-runner hooks.
- **Sinks** — one translation unit each: `NoneSink`
  ([none_sink.h](none_sink.h)), `MemorySink` ([memory_sink.h](memory_sink.h)),
  `FileSink` ([file_sink.cc](file_sink.cc), [file_sink.h](file_sink.h)),
  `FbDevSink` ([fbdev_sink.cc](fbdev_sink.cc), [fbdev_sink.h](fbdev_sink.h)),
  `DrmDumbSink` ([drm_dumb_sink.cc](drm_dumb_sink.cc),
  [drm_dumb_sink.h](drm_dumb_sink.h)). See the Sinks table below.
- **`SinkFactory`** ([sink_factory.cc](sink_factory.cc),
  [sink_factory.h](sink_factory.h)) — parses the `IVI_SW_SINK` spec and
  builds the active sink; unrecognized specs warn and fall back to `NoneSink`.
- **`pixel_swizzle.h`** ([pixel_swizzle.h](pixel_swizzle.h)) — the pack
  helpers: `FlutterToBGRX8888` (XRGB, single-pass memcpy + alpha-force),
  `FlutterToRGB565` / `FlutterToRGB565_BayerDither`.
- **`SoftwareSeat`** ([input/software_seat.cc](input/software_seat.cc),
  [input/software_seat.h](input/software_seat.h),
  [input/libinput_log_bridge.h](input/libinput_log_bridge.h)) — libinput
  `ISeat` driving pointer / keyboard / key-repeat / multi-touch into Flutter.
- **`SoftwareCursor`** ([software_cursor.cc](software_cursor.cc),
  [software_cursor.h](software_cursor.h)) — shared cursor bitmap composited
  by the dumb sink.

### Sinks

| Sink | Spec syntax | What it does | Vsync |
|---|---|---|---|
| **NoneSink** | `none` (default) | Discards every frame. CI engine-only smoke. | — |
| **MemorySink** | `memory` | Mutex-guarded `std::vector<uint8_t>` snapshot of the most recent frame. `SnapshotLatest(&row_bytes, &height)` exposes it to in-process test fixtures. | — |
| **FileSink** | `file:<path-pattern>` | Writes each frame as a NetPBM PAM (P7) file. Pattern with `%d` / `%05d` interpolates the frame index. Pattern without `%` writes the first frame only. Parent directories auto-created. | — |
| **FbDevSink** | `fbdev[:<device>]` | Opens `/dev/fb0` (or operator path), mmaps, packs each Present. Auto-detects 32-bpp BGRA/BGRX or 16-bpp RGB565 from the driver's `FBIOGET_VSCREENINFO` offsets; refuses palettized / `nonstd != 0` / other layouts with a clear error. | — |
| **DrmDumbSink** | `drm-dumb[:<device>]` | Opens `/dev/dri/card0`, picks first connected connector + its CRTC + preferred mode, allocates 2 dumb buffers (XRGB8888 by default, RGB565 via `IVI_SW_DRM_FORMAT=rgb565` when the plane advertises it), modesets onto buffer 0. Per-frame: pack into the back buffer, `drmModePageFlip`. PAGE_FLIP_EVENT drives Flutter's `vsync_callback` with the kernel-provided scanout timestamp. | ✓ |

`drm-dumb` is the only sink that advertises `SupportsVsync()`;
`SoftwareBackend::GetVsyncCallback()` returns a trampoline iff the
active sink advertises it. Everyone else runs on Flutter's wall-clock
scheduler.

### File map

- [software_backend.cc](software_backend.cc), [software_backend.h](software_backend.h) — the `SoftwareBackend` `Backend`: kSoftware render config, present trampoline, sink ownership, extent resolution, vsync gate, profile summary.
- [surface_sink.h](surface_sink.h) — the `ISurfaceSink` present abstraction.
- [none_sink.h](none_sink.h) — `NoneSink` (drop).
- [memory_sink.h](memory_sink.h) — `MemorySink` (latest-frame snapshot).
- [file_sink.cc](file_sink.cc), [file_sink.h](file_sink.h) — `FileSink` (PAM goldens).
- [fbdev_sink.cc](fbdev_sink.cc), [fbdev_sink.h](fbdev_sink.h) — `FbDevSink` (fbdev mmap + swizzle).
- [drm_dumb_sink.cc](drm_dumb_sink.cc), [drm_dumb_sink.h](drm_dumb_sink.h) — `DrmDumbSink` (dumb buffer page-flip + vsync).
- [sink_factory.cc](sink_factory.cc), [sink_factory.h](sink_factory.h) — `IVI_SW_SINK` spec parse → sink build.
- [pixel_swizzle.h](pixel_swizzle.h) — BGRA→XRGB / RGB565 pack helpers (+ Bayer dither).
- [software_cursor.cc](software_cursor.cc), [software_cursor.h](software_cursor.h) — shared cursor bitmap composited by the dumb sink.
- [input/software_seat.cc](input/software_seat.cc), [input/software_seat.h](input/software_seat.h) — libinput `SoftwareSeat`.
- [input/libinput_log_bridge.h](input/libinput_log_bridge.h) — libinput → project log bridge.

### Threading model

- **Single rasterizer thread**: every sink's `Present()` is called from
  Flutter's rasterizer. `FbDevSink` and `DrmDumbSink` rely on that for
  lock-free hot paths; the dtor contract is that the embedder must join the
  rasterizer (via `FlutterEngineDeinitialize` + `Shutdown`) before dropping
  the sink. `FlutterView`'s dtor enforces this by calling `StopVsyncMonitor`
  before `m_flutter_engine` destructs.
- **Platform task runner**: `DrmDumbSink`'s PAGE_FLIP_EVENT arrives on the
  platform task runner via asio `async_wait` on the drm fd (mirrors
  `drm_kms_egl`'s pattern); the handler exchanges the parked vsync baton and
  posts `FlutterEngineOnVsync` onto the engine's strand with the
  kernel-provided scanout timestamp. A `SetVsyncBaton` idle-kick drains the
  baton inline when no flip is in flight so Flutter never sits forever on the
  first frame or post-resume.
- **`SoftwareSeat`**: libinput fd + key-repeat timerfd polled on the
  `SoftwareDisplay` input path, started/stopped via
  `IDisplay::StartEvents()` / `StopEvents()`.

---

## Build steps

### Dependencies

- CMake + Ninja + a C++ toolchain (no GPU, no Wayland, no Mesa runtime).
- `libdrm` (pkg-config) — optional, only for the `drm-dumb` sink.
- `linux/fb.h` — ships with every libc (fbdev sink needs no library dep).
- `libinput` + `libudev` + `xkbcommon` (pkg-config) — optional, only for the
  `SoftwareSeat`.

### Configure + build

```sh
cmake -B build -G Ninja \
  -DBUILD_BACKEND_SOFTWARE=ON \
  -DBUILD_BACKEND_WAYLAND_EGL=OFF \
  -DBUILD_BACKEND_WAYLAND_VULKAN=OFF \
  -DBUILD_BACKEND_DRM_KMS_EGL=OFF
ninja -C build
```

Backend selection is mutually exclusive — `BUILD_BACKEND_SOFTWARE`
must be on with everything else off, enforced by the top-level
CMakeLists.

### Build matrix

| Config | CMake flags | Present path compiled in |
|--------|-------------|--------------------------|
| Headless / CI (always) | `-DBUILD_BACKEND_SOFTWARE=ON` | `NoneSink`, `MemorySink`, `FileSink` — always compiled in |
| + fbdev panel | `-DBUILD_SOFTWARE_SINK_FBDEV=ON` (default) | `FbDevSink` (`/dev/fb*` mmap) |
| + DRM dumb buffer | `-DBUILD_SOFTWARE_SINK_DRM=ON` (auto if libdrm) | `DrmDumbSink` (page-flip + vsync) |
| + libinput input | `-DBUILD_SOFTWARE_INPUT_LIBINPUT=ON` (auto if deps) | `SoftwareSeat` |

Sink / input sub-options:

| CMake flag | Default | Notes |
|---|---|---|
| `BUILD_SOFTWARE_SINK_DRM` | auto-on if pkg-config finds `libdrm`, else off | Pulls in `libdrm` for the drm-dumb sink. Force-on without libdrm is a fatal configure error. |
| `BUILD_SOFTWARE_SINK_FBDEV` | ON | No library dep — `linux/fb.h` ships with every libc. |
| `BUILD_SOFTWARE_INPUT_LIBINPUT` | auto-on if pkg-config finds `libinput` + `libudev` + `xkbcommon`, else off | Pulls in the libinput stack for the `SoftwareSeat`. Force-on without deps is a fatal configure error. |

`NoneSink`, `MemorySink`, and `FileSink` are always compiled in.

### Optional features detected at configure time

- **libdrm** present → `BUILD_SOFTWARE_SINK_DRM` auto-on, `DrmDumbSink`
  compiled in; absent → sink omitted (`drm-dumb` specs fall back to `NoneSink`).
- **libinput + libudev + xkbcommon** present → `BUILD_SOFTWARE_INPUT_LIBINPUT`
  auto-on, `SoftwareSeat` compiled in; absent → no CPU-backend input.

---

## Running

The backend needs no display server and no GPU. Sinks that touch device
nodes need group membership:

- `fbdev` → the `video` group (or whatever owns `/dev/fb*` on the target).
- `drm-dumb` → the `video` (or distro-specific) group; needs DRM master on
  the card.
- `SoftwareSeat` input → the `input` group (universal on systemd distros) or
  root; devices are opened directly under the calling process's credentials
  via raw `::open`, no libseat dependency.

### CI / engine-only smoke

```sh
IVI_SW_STOP_AFTER_FRAMES=5 \
  ./homescreen -b path/to/bundle/.desktop-homescreen
# → engine boots, Dart runs to /home, exits SIGTERM after 5 frames.
```

### Golden capture

```sh
IVI_SW_SINK=file:out/frame_%05d.pam \
IVI_SW_STOP_AFTER_FRAMES=5 \
  ./homescreen -b path/to/bundle/.desktop-homescreen
# → out/frame_00000.pam .. frame_00004.pam (RGBA8888, viewable in
#   ImageMagick / GIMP / feh).
```

For a single-shot capture (steady-state frame after N frames have
stabilised):

```sh
IVI_SW_SINK=file:out/initial.pam IVI_SW_STOP_AFTER_FRAMES=60 ./homescreen ...
```

The `file:` path with no `%` placeholder only writes the first frame;
combined with `IVI_SW_STOP_AFTER_FRAMES=60` you get the very first
present, then 59 more present cycles to let the bundle settle (those
59 are noops on disk), then SIGTERM.

### fbdev panel

```sh
IVI_SW_SINK=fbdev:/dev/fb0 ./homescreen -b bundle
```

User must be in the `video` group (or whatever owns `/dev/fb*` on the
target). Two pixel layouts are accepted, auto-detected from
`FBIOGET_VSCREENINFO`:

* 32-bpp BGRA/BGRX (R@16, G@8, B@0, `nonstd=0`) — the universal
  modern fbdev surface.
* 16-bpp RGB565 (R@11/5, G@5/6, B@0/5, `nonstd=0`) — common on
  cost-sensitive embedded panels.

Anything else (palettized, vendor-`nonstd`, or unrecognised
offsets) prints an error line on startup pointing at the actual
bpp / R/G/B offsets, and the sink falls back to `NoneSink`.

For a Linux dev host without a real fbdev, the kernel `vfb` module
synthesises one:

```sh
sudo modprobe vfb vfb_enable=1
# /dev/fb0 appears at 800x600 32-bpp BGRA by default.
# Force RGB565 with: vfb_enable=1 video=vfb:1024x768-16
```

`IVI_SW_DRM_DITHER=1` adds Bayer 4×4 dithering to the RGB565 pack
to hide banding on smooth gradients.

### DRM dumb buffer + vsync

```sh
IVI_SW_SINK=drm-dumb:/dev/dri/card0 ./homescreen -b bundle
```

User must be in the `video` (or distro-specific) group. The sink:

* finds the first connected connector with at least one mode;
* prefers `DRM_MODE_TYPE_PREFERRED`, else mode[0];
* picks the connector's currently-bound encoder/CRTC (falling back
  to `possible_crtcs`);
* snapshots the prior CRTC binding so the dtor restores the console
  on exit;
* allocates 2 dumb buffers in the format selected by
  `IVI_SW_DRM_FORMAT` (default `xrgb8888` at 32 bpp; `rgb565` at
  16 bpp when the picked CRTC's planes advertise it — falls back to
  XRGB with a warn otherwise);
* modesets onto buffer 0;
* per frame: packs Flutter's BGRA into the back buffer via
  `pixel_swizzle.h::FlutterToBGRX8888` (XRGB, single-pass memcpy +
  alpha-force) or `FlutterToRGB565` /
  `FlutterToRGB565_BayerDither` (RGB565), queues `drmModePageFlip`
  with `DRM_MODE_PAGE_FLIP_EVENT`.

RGB565 example:

```sh
IVI_SW_DRM_FORMAT=rgb565 IVI_SW_DRM_DITHER=1 \
  IVI_SW_SINK=drm-dumb:/dev/dri/card0 ./homescreen -b bundle
```

PAGE_FLIP_EVENT arrives on the platform task runner via asio
`async_wait` on the drm fd (mirrors `drm_kms_egl`'s pattern), the
handler exchanges the parked vsync baton and posts
`FlutterEngineOnVsync` onto the engine's strand with the kernel-
provided scanout timestamp.

A `SetVsyncBaton` idle-kick drains the baton inline when no flip is
in flight so Flutter never sits forever on the first frame or
post-resume.

### Env vars

| Env | Default | Effect |
|---|---|---|
| `IVI_SW_SINK` | `none` | Pick the active sink at startup. Syntax above. Unrecognized specs log a warn and fall back to `NoneSink` so a CI typo never refuses to start. |
| `IVI_SW_STOP_AFTER_FRAMES` | (off) | Raise `SIGTERM` after N successful presents. Lets CI bound runtime by frame count instead of wall-clock. Bare integer ≥ 1; leading `-`/`+` is rejected and logged. First crosser latches via `compare_exchange` so the signal fires exactly once. |
| `IVI_SW_VSYNC` | `1` (on) | `0` forces Flutter onto its wall-clock scheduler regardless of whether the active sink advertises `SupportsVsync()`. Useful for A/B benchmarking the vsync_callback contribution (see benchmarks section). |
| `IVI_SW_PROFILE` | (off) | Enable `SoftwareBackend` **present-call** cadence profiling (when Flutter hands the backend a rendered frame). Every 60 frames logs `[SoftwareBackend] profile (n=60): fps=X mean_interval=Yus max_interval=Zus discarded=N buckets[60Hz/30Hz/20Hz/slow/idle]=…`. Session summary on clean dtor. Same shape as `IVI_VK_PROFILE` / `IVI_WL_PROFILE` for cross-backend comparison. |
| `IVI_VSYNC_PROFILE` | (off) | Enable **vsync/scanout** cadence profiling for the shared `IVsyncProvider` (drm-dumb sink: from the kernel page-flip timestamp). Logs under the `[SoftwareVsync]` label, same window/bucket shape. Display-side companion to `IVI_SW_PROFILE`'s rasterizer-side numbers; the same var also gates `[WaylandVsync]` / `[DrmVsync]` on those backends. |
| `IVI_SW_INPUT` | `auto` | Wires the libinput-backed `SoftwareSeat` for device targets. Set to `none` to skip — useful for CI runs that lack `/dev/input/event*` or want pure engine-only smoke. |
| `IVI_SW_DRM_FORMAT` | `xrgb8888` | Pick the `DrmDumbSink` buffer format. `rgb565` halves framebuffer footprint and CRTC scanout bandwidth (the real bottleneck on legacy SoCs like TI AM335x / STM32MP1). If the picked CRTC's planes don't advertise RGB565, the sink warns and falls back to XRGB. Unrecognized values warn and fall back. Has no effect on `fbdev:` (auto-detected from the panel) or the other sinks. |
| `IVI_SW_DRM_DITHER` | `0` (off) | `1` enables Bayer 4×4 ordered dithering on the RGB565 pack path in both `drm-dumb` and `fbdev` sinks. Hides the banding that pure truncation produces on smooth gradients at the cost of bit-exact goldens. No-op when the active sink's format is BGRX8888 — there's no precision loss to hide. |

---

## Input

`SoftwareSeat` is a libinput-backed `homescreen::ISeat` that drives
keyboard, pointer, and multi-touch events into Flutter from
`/dev/input/event*`. Owned by `SoftwareDisplay` (parallel to how
`DrmDisplay` owns `DrmSeat`) and started/stopped via the existing
`IDisplay::StartEvents()` / `StopEvents()` lifecycle.

Devices are opened under the calling process's credentials via raw
`::open` — no libseat dependency. The operator must be in the
`input` group (universal on systemd distros) or running as root.

Coverage:

| Source | What it does |
|---|---|
| Pointer | Relative + absolute motion clamped to the viewport, BTN_LEFT / RIGHT / MIDDLE / SIDE / EXTRA buttons, horizontal + vertical scroll axes. |
| Keyboard | evdev keycode + 8 → xkb scancode via `xkb_state_key_get_one_sym`; system-default RMLVO (`XKB_DEFAULT_*` env vars honoured). Delivered through the project-wide `KeyCallback`. |
| Key repeat | timerfd polled alongside libinput's fd; armed on press of an `xkb_keymap_key_repeats`-positive key at 500 ms / 33 ms (~30 Hz) cadence, disarmed on release of the currently-repeating key. Most-recently-pressed wins when a second repeating key arrives. |
| Touch | libinput slots → Flutter per-finger `device` ids. Per slot: `kAdd + kDown` on touch-down, `kMove` on motion, `kUp + kRemove` on release, `kCancel + kRemove` on libinput cancellation. Bounded to 16 slots. |

Not yet implemented: gesture events (`LIBINPUT_EVENT_GESTURE_*`),
switch events (`LIBINPUT_EVENT_SWITCH_*`), tablet/pen, hot keymap
reload, libseat session integration.

---

## Diagnostics/Debug

Present-side and scanout-side cadence are observable via the profilers:
`IVI_SW_PROFILE=1` logs the rasterizer-side present cadence under
`[SoftwareBackend]`, and `IVI_VSYNC_PROFILE=1` logs the drm-dumb kernel
page-flip cadence under `[SoftwareVsync]` (see Env vars for the line shape
and bucket thresholds).

### Lifecycle + safety

* **Single rasterizer thread**: every sink's `Present()` is called
  from Flutter's rasterizer. `FbDevSink` and `DrmDumbSink` rely on
  that for lock-free hot paths; the dtor contract is that the
  embedder must join the rasterizer (via `FlutterEngineDeinitialize`
  + `Shutdown`) before dropping the sink. `FlutterView`'s dtor
  enforces this by calling `StopVsyncMonitor` before `m_flutter_engine`
  destructs.
* **`DrmDumbSink` flip stall**: the spin-wait on `flip_pending_`
  has a deadline of 5× the refresh period; if the kernel never fires
  the event we force-clear and warn rather than hanging the rasterizer.
* **`DrmDumbSink` flip-queue failure**: when `drmModePageFlip` is
  rejected the parked baton is drained inline with wall-clock-now
  instead of leaking on the floor.
* **`DrmDumbSink::SetPlatformTaskRunner` re-entry**: guards against
  a future double-arm closing the drm fd underneath us (asio's 2-arg
  `stream_descriptor` ctor adopts the fd).
* **`SoftwareBackend::IVI_SW_STOP_AFTER_FRAMES`** raises `SIGTERM`
  from the rasterizer thread via `std::raise` (async-signal-safe).
  The existing `main.cc` handler flips a `sig_atomic_t` flag, the
  trampoline unwinds, `App::Loop` observes the flag and returns
  normally — no hard exit, no torn destructor state.

---

## Benchmarks (DrmDumbSink, vkms @ 60 Hz)

Measured against the kernel's `vkms` virtual KMS module — `card0` is
a vkms device exposing a 1024×768 @ 60 Hz Virtual-1 connector with a
Writeback-2 sink. Reproducible without real hardware: `sudo modprobe
vkms` adds the device, the rest of the embedder talks to it through
the same `drmModePageFlip` path it would use on a real panel.

### Methodology

`IVI_SW_PROFILE=1` adds a log line every 60 presented frames with
mean / max interval, discarded-frame count, and a 5-bucket histogram
of per-frame intervals against a 60 Hz baseline (≤17 ms = on-vblank,
18-33 ms = 1 vblank missed, 34-50 ms = 2 vblanks missed, 51-100 ms =
slow, >100 ms = idle / pause). Identical bucket thresholds to the
`drm_kms_egl` / `wayland_egl` / `wayland_vulkan` profilers so
histograms line up across backends.

Workload: the flutter-wonderous-app debug bundle. Three runs per
configuration. Steady-state windows reported (windows dominated by
content-load stalls — see "vkms quirk" below — are excluded for the
typical-case table).

### Results summary

**Steady-state, vsync ON** —
`IVI_SW_SINK=drm-dumb IVI_SW_PROFILE=1 IVI_SW_STOP_AFTER_FRAMES=240`:

| Run | fps | mean interval | 60Hz (≤17 ms) | 30Hz (18-33 ms) | 20Hz | slow | idle |
|---|---:|---:|---:|---:|---:|---:|---:|
| r1 w1 | 57.20 | 17.48 ms | 37 | 22 | 0 | 0 | 0 |
| r2 w1 | 57.15 | 17.50 ms | 39 | 20 | 0 | 0 | 0 |
| r3 w1 | 55.96 | 17.87 ms | 37 | 22 | 0 | 0 | 0 |

**Steady-state, vsync OFF** — same workload, `IVI_SW_VSYNC=0` forcing
Flutter to wall-clock scheduling regardless of the sink's
`SupportsVsync()`:

| Run | fps | mean interval | 60Hz (≤17 ms) | 30Hz (18-33 ms) | 20Hz | slow | idle |
|---|---:|---:|---:|---:|---:|---:|---:|
| r1 w1 | 56.35 | 17.75 ms | 39 | 20 | 0 | 0 | 0 |
| r2 w1 | 55.62 | 17.98 ms | 41 | 18 | 0 | 0 | 0 |
| r3 w1 | 54.26 | 18.43 ms | 30 | 27 | 2 | 0 | 0 |
| r3 w4 | 58.34 | 17.14 ms | 50 | 10 | 0 | 0 | 0 |

**Reading.** The two configurations are **statistically
indistinguishable** at the histogram layer — ~50-83 % on-vblank,
~17-50 % one-vblank-late, mean interval within ~0.3 ms of the 16.67 ms
vblank period in both cases. The DrmDumbSink's spin-wait on
`flip_pending_` paces the rasterizer thread to the kernel's
PAGE_FLIP_EVENT cadence *regardless* of whether Flutter's
`vsync_callback` is also wired — the sink's natural backpressure
dominates the present-interval metric.

This matches the wayland_vulkan FIFO finding documented in
`shell/backend/wayland_vulkan/README.md`: when the sink (or WSI)
imposes strict vblank gating, the `vsync_callback` contribution
becomes invisible at the histogram level. Both runs lock the
rasterizer to compositor cadence; what the env var toggle changes
is whether Flutter's `beginFrame` timestamp comes from the kernel
or from a wall-clock estimate. Animation-timing precision and
input-to-photon latency may differ in ways this profiler doesn't
measure; the present-interval rate doesn't.

**vkms quirk worth knowing.** Longer runs occasionally show a window
with `fps≈3` and a `max_interval` north of 10 seconds (vsync-off r3 w2
in the matrix above hit ~18 s). This is the kernel's vkms module
pausing the writeback queue when no consumer is reading from the
`card0-Writeback-2` connector — PAGE_FLIP_EVENT stops arriving,
the rasterizer parks on `flip_pending_`, and the spin-wait
eventually trips its 5×-refresh deadline and force-clears the flag.
The histogram exits the stall correctly (vsync-off r3 w4 recovers
to ~58 fps), but a CI run hitting this on a longer bundle would
see the same artefact. Not a regression — same behaviour with
vsync wiring off. On a real panel with a connected scanout
consumer this doesn't occur.

**Cross-backend comparison.** Per-vblank hit rate on the wonderous
bundle at 60 Hz:

| Backend | 60Hz on-vblank | Mean interval | Notes |
|---|---:|---:|---|
| `drm_kms_egl` (RT + vsync, 240 Hz panel) | 98.0 % | 4.31 ms | direct-scanout fast path |
| `wayland_egl` (vsync, KWin 60 Hz) | 97.46 % | 17.64 ms | wp_presentation_feedback |
| `wayland_vulkan` FIFO (vsync, KWin 60 Hz) | 70-73 % | ~17.5 ms | Mesa WSI strict gating |
| **`software` drm-dumb (vsync, vkms 60 Hz)** | **61-66 %** | **17.6 ms** | **CPU memcpy + page-flip** |
| `software` drm-dumb (no vsync, vkms 60 Hz) | 50-83 % | 17.8 ms | ≈ identical to vsync-on |

The software backend's bucket dispersion is wider than
wayland_egl's because the rasterizer + memcpy + DRM page-flip path
shares the same vblank budget as Flutter's CPU render — any small
scheduling jitter lands frames in the 30Hz bucket. The per-frame
CPU work on the present side is dominated by Flutter's raster, not
the swizzle: `pixel_swizzle.h::FlutterToBGRX8888` on LE folds the
memcpy + alpha-force into a single SIMD pass (~16-byte SSE2 stride,
or 32-byte AVX2), so 1024×768 BGRA→XRGB is well under 1 ms on a
modern host. A panel with a faster CPU or smaller dimensions would
push the histogram toward wayland_egl's profile.

---

## Known limitations

- **No platform-view compositor.** `GetCompositorConfig()` returns
  a struct with all callbacks null, so the engine uses the
  non-compositor `surface_present_callback` path for the entire
  scene. Layer interleaving with platform views would need a
  software compositor (one CPU-allocated buffer per layer + blend
  on the rasterizer). Doable, not done.
- **fbdev / DRM pixel formats** are limited to 32-bpp BGRA/BGRX
  and 16-bpp RGB565. RGB555, packed-YUV, 10-bpc (XRGB2101010), and
  palettized layouts would each need a dedicated pack helper in
  `pixel_swizzle.h`. RGB555 is the closest existing case
  (`FlutterToRGB565` is the template; mask widths shift by one bit
  on R/B and G drops from 6 to 5).
- **`DrmDumbSink` picks the first connected connector** and its
  currently-bound CRTC. Multi-output panels, choosing by name
  (`HDMI-A-1`), or explicit mode selection aren't yet exposed.
  The sink-spec syntax has room — `drm-dumb:/dev/dri/card0,connector=HDMI-A-1,mode=1920x1080@60`
  would parse cleanly when the use case shows up.
- **Input coverage is partial.** `SoftwareSeat` (see *Input* above)
  handles pointer, keyboard, key-repeat, and multi-touch via
  libinput. Not yet wired: gesture events (`LIBINPUT_EVENT_GESTURE_*`),
  switch events, tablet/pen, hot keymap reload, libseat session
  integration (devices are opened directly under the calling
  process's credentials).
- **fbdev has no vsync**. `FBIO_WAITFORVSYNC` is broadly broken
  across drivers and not standardized; the sink doesn't expose it.
  For real vsync on a CPU-only SoC, use `drm-dumb` instead.
- **`DrmDumbSink` refresh from `vrefresh`** is integer Hz; modes
  that report 59.94 round to 60 and Flutter's deadline drifts
  ~one frame per 17 minutes. If drift matters, switch to
  `mode.clock * 1000 / (htotal * vtotal)`.
- **BE host (big-endian)** isn't tested. `pixel_swizzle.h` keeps a
  correct branch for DRM XRGB (byte order is endian-invariant per
  drm_fourcc.h), but fbdev with `red.offset=16` lands at memory
  `[X,R,G,B]` on BE rather than `[B,G,R,X]` — `FbDevSink` would
  need a dedicated helper. Not implemented since all shipping
  targets are LE; flagged for the day a BE target appears.

---

## References

- [backend/backend.h](../backend.h) — the `Backend` interface `SoftwareBackend` implements.
- [shell/backend/README.md](../README.md) — backend selection overview.
- [drm_kms_egl](../drm_kms_egl/README.md) — the drm fd / asio page-flip pattern `DrmDumbSink` mirrors.
- [wayland_vulkan](../wayland_vulkan/README.md) — the FIFO / strict-vblank-gating finding cross-referenced in Benchmarks.
- [shell/vsync](../../vsync/README.md) — the shared `IVsyncProvider` baton lifecycle.
- [shell/profiling](../../profiling/README.md) — `FrameProfile` and the `IVI_*_PROFILE` cadence histograms.
- [shell/input](../../input/README.md) — the sibling `DrmSeat` libinput/xkb stack `SoftwareSeat` parallels.
- Linux `linux/fb.h` (fbdev), `libdrm` / `drm_fourcc.h` (dumb buffers), `libinput` + `xkbcommon` (input), `vkms` / `vfb` kernel modules (test harness).
