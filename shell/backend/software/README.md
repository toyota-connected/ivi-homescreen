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

## Architecture

```
Flutter Engine                SoftwareBackend                 ISurfaceSink
──────────────                ───────────────                 ────────────
software.surface_present_     PresentTrampoline() ───▶        Present(buf, rb, h)
  callback(buf, rb, h)                                          ├─ NoneSink     (drop)
                                                                ├─ MemorySink   (memcpy → vector)
                                                                ├─ FileSink     (write PAM)
                                                                ├─ FbDevSink    (mmap + swizzle)
                                                                └─ DrmDumbSink  (page-flip + vsync)
```

* `SoftwareBackend` is the Flutter-facing surface — implements
  `GetRenderConfig` with `type=kSoftware` plus a static present
  trampoline. Owns the sink.
* `ISurfaceSink` is the abstraction: one `Present(const void*,
  size_t row_bytes, size_t height)` plus optional vsync hooks. Sinks
  are picked at startup from the `IVI_SW_SINK` env var.
* Sinks are single-file and small: no threading, no buffer pooling.
  The callback runs on Flutter's rasterizer thread; the sink either
  copies / writes / mmaps synchronously and returns.

`SoftwareDisplay` is a no-op `IDisplay` so `App::Loop`'s sleep math
has a refresh-rate denominator. Defaults to 60 Hz; nothing else
interesting lives there.

## Sinks

| Sink | Spec syntax | What it does | Vsync |
|---|---|---|---|
| **NoneSink** | `none` (default) | Discards every frame. CI engine-only smoke. | — |
| **MemorySink** | `memory` | Mutex-guarded `std::vector<uint8_t>` snapshot of the most recent frame. `SnapshotLatest(&row_bytes, &height)` exposes it to in-process test fixtures. | — |
| **FileSink** | `file:<path-pattern>` | Writes each frame as a NetPBM PAM (P7) file. Pattern with `%d` / `%05d` interpolates the frame index. Pattern without `%` writes the first frame only. Parent directories auto-created. | — |
| **FbDevSink** | `fbdev[:<device>]` | Opens `/dev/fb0` (or operator path), validates 32-bpp BGRA/BGRX, mmaps, memcpy+swizzles each Present. Refuses RGB565 / palettized / `nonstd != 0` with a clear error. | — |
| **DrmDumbSink** | `drm-dumb[:<device>]` | Opens `/dev/dri/card0`, picks first connected connector + its CRTC + preferred mode, allocates 2 dumb buffers, modesets onto buffer 0. Per-frame: swizzle into the back buffer, `drmModePageFlip`. PAGE_FLIP_EVENT drives Flutter's `vsync_callback` with the kernel-provided scanout timestamp. | ✓ |

`drm-dumb` is the only sink that advertises `SupportsVsync()`;
`SoftwareBackend::GetVsyncCallback()` returns a trampoline iff the
active sink advertises it. Everyone else runs on Flutter's wall-clock
scheduler.

## Env vars

| Env | Default | Effect |
|---|---|---|
| `IVI_SW_SINK` | `none` | Pick the active sink at startup. Syntax above. Unrecognized specs log a warn and fall back to `NoneSink` so a CI typo never refuses to start. |
| `IVI_SW_STOP_AFTER_FRAMES` | (off) | Raise `SIGTERM` after N successful presents. Lets CI bound runtime by frame count instead of wall-clock. Bare integer ≥ 1; leading `-`/`+` is rejected and logged. First crosser latches via `compare_exchange` so the signal fires exactly once. |
| `IVI_SW_VSYNC` | `1` (on) | `0` forces Flutter onto its wall-clock scheduler regardless of whether the active sink advertises `SupportsVsync()`. Useful for A/B benchmarking the vsync_callback contribution (see benchmarks section). |
| `IVI_SW_PROFILE` | (off) | Enable per-frame cadence profiling. Every 60 frames logs `profile (n=60): fps=X mean_interval=Yus max_interval=Zus present_failures=N buckets[60Hz/30Hz/20Hz/slow/idle]=…`. Session summary on clean dtor. Same shape as `IVI_VK_PROFILE` / `IVI_WL_PROFILE` for cross-backend comparison. |
| `IVI_SW_INPUT` | `auto` | Wires the libinput-backed `SoftwareSeat` for device targets. Set to `none` to skip — useful for CI runs that lack `/dev/input/event*` or want pure engine-only smoke. |

## Build

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

### Sink sub-options

| CMake flag | Default | Notes |
|---|---|---|
| `BUILD_SOFTWARE_SINK_DRM` | auto-on if pkg-config finds `libdrm`, else off | Pulls in `libdrm` for the drm-dumb sink. Force-on without libdrm is a fatal configure error. |
| `BUILD_SOFTWARE_SINK_FBDEV` | ON | No library dep — `linux/fb.h` ships with every libc. |
| `BUILD_SOFTWARE_INPUT_LIBINPUT` | auto-on if pkg-config finds `libinput` + `libudev` + `xkbcommon`, else off | Pulls in the libinput stack for the `SoftwareSeat`. Force-on without deps is a fatal configure error. |

`NoneSink`, `MemorySink`, and `FileSink` are always compiled in.

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

## Running

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
target). Pixel format check is strict — if the panel exposes
RGB565 / palettized / vendor-`nonstd` you'll see an error line on
startup pointing at the actual bpp / R/G/B offsets, and the sink
falls back to `NoneSink`.

For a Linux dev host without a real fbdev, the kernel `vfb` module
synthesises one:

```sh
sudo modprobe vfb vfb_enable=1
# /dev/fb0 appears at 800x600 32-bpp BGRA by default.
```

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
* allocates 2 `DRM_FORMAT_XRGB8888` dumb buffers and modesets onto
  buffer 0;
* per frame: swizzles RGBA→XRGB into the back buffer, queues
  `drmModePageFlip` with `DRM_MODE_PAGE_FLIP_EVENT`.

PAGE_FLIP_EVENT arrives on the platform task runner via asio
`async_wait` on the drm fd (mirrors `drm_kms_egl`'s pattern), the
handler exchanges the parked vsync baton and posts
`FlutterEngineOnVsync` onto the engine's strand with the kernel-
provided scanout timestamp.

A `SetVsyncBaton` idle-kick drains the baton inline when no flip is
in flight so Flutter never sits forever on the first frame or
post-resume.

## Lifecycle + safety

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

## Benchmarks (DrmDumbSink, vkms @ 60 Hz)

Measured against the kernel's `vkms` virtual KMS module — `card0` is
a vkms device exposing a 1024×768 @ 60 Hz Virtual-1 connector with a
Writeback-2 sink. Reproducible without real hardware: `sudo modprobe
vkms` adds the device, the rest of the embedder talks to it through
the same `drmModePageFlip` path it would use on a real panel.

### Methodology

`IVI_SW_PROFILE=1` adds a log line every 60 presented frames with
mean / max interval, present-failure count, and a 5-bucket histogram
of per-frame intervals against a 60 Hz baseline (≤17 ms = on-vblank,
18-33 ms = 1 vblank missed, 34-50 ms = 2 vblanks missed, 51-100 ms =
slow, >100 ms = idle / pause). Identical bucket thresholds to the
`drm_kms_egl` / `wayland_egl` / `wayland_vulkan` profilers so
histograms line up across backends.

Workload: the flutter-wonderous-app debug bundle. Three runs per
configuration. Steady-state windows reported (windows dominated by
content-load stalls — see "vkms quirk" below — are excluded for the
typical-case table).

### Steady-state, vsync ON

`IVI_SW_SINK=drm-dumb IVI_SW_PROFILE=1 IVI_SW_STOP_AFTER_FRAMES=240`:

| Run | fps | mean interval | 60Hz (≤17 ms) | 30Hz (18-33 ms) | 20Hz | slow | idle |
|---|---:|---:|---:|---:|---:|---:|---:|
| r1 w1 | 57.20 | 17.48 ms | 37 | 22 | 0 | 0 | 0 |
| r2 w1 | 57.15 | 17.50 ms | 39 | 20 | 0 | 0 | 0 |
| r3 w1 | 55.96 | 17.87 ms | 37 | 22 | 0 | 0 | 0 |

### Steady-state, vsync OFF

Same workload, `IVI_SW_VSYNC=0` forcing Flutter to wall-clock
scheduling regardless of the sink's `SupportsVsync()`:

| Run | fps | mean interval | 60Hz (≤17 ms) | 30Hz (18-33 ms) | 20Hz | slow | idle |
|---|---:|---:|---:|---:|---:|---:|---:|
| r1 w1 | 56.35 | 17.75 ms | 39 | 20 | 0 | 0 | 0 |
| r2 w1 | 55.62 | 17.98 ms | 41 | 18 | 0 | 0 | 0 |
| r3 w1 | 54.26 | 18.43 ms | 30 | 27 | 2 | 0 | 0 |
| r3 w4 | 58.34 | 17.14 ms | 50 | 10 | 0 | 0 | 0 |

### Reading

The two configurations are **statistically indistinguishable** at
the histogram layer — ~50-83 % on-vblank, ~17-50 % one-vblank-late,
mean interval within ~0.3 ms of the 16.67 ms vblank period in both
cases. The DrmDumbSink's spin-wait on `flip_pending_` paces the
rasterizer thread to the kernel's PAGE_FLIP_EVENT cadence
*regardless* of whether Flutter's `vsync_callback` is also wired —
the sink's natural backpressure dominates the present-interval
metric.

This matches the wayland_vulkan FIFO finding documented in
`shell/backend/wayland_vulkan/README.md`: when the sink (or WSI)
imposes strict vblank gating, the `vsync_callback` contribution
becomes invisible at the histogram level. Both runs lock the
rasterizer to compositor cadence; what the env var toggle changes
is whether Flutter's `beginFrame` timestamp comes from the kernel
or from a wall-clock estimate. Animation-timing precision and
input-to-photon latency may differ in ways this profiler doesn't
measure; the present-interval rate doesn't.

### vkms quirk worth knowing

Longer runs occasionally show a window with `fps≈3` and a
`max_interval` north of 10 seconds (vsync-off r3 w2 in the matrix
above hit ~18 s). This is the kernel's vkms module pausing the
writeback queue when no consumer is reading from the
`card0-Writeback-2` connector — PAGE_FLIP_EVENT stops arriving,
the rasterizer parks on `flip_pending_`, and the spin-wait
eventually trips its 5×-refresh deadline and force-clears the flag.
The histogram exits the stall correctly (vsync-off r3 w4 recovers
to ~58 fps), but a CI run hitting this on a longer bundle would
see the same artefact. Not a regression — same behaviour with
vsync wiring off. On a real panel with a connected scanout
consumer this doesn't occur.

### Cross-backend comparison

Per-vblank hit rate on the wonderous bundle at 60 Hz:

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

## Known limitations

1. **No platform-view compositor.** `GetCompositorConfig()` returns
   a struct with all callbacks null, so the engine uses the
   non-compositor `surface_present_callback` path for the entire
   scene. Layer interleaving with platform views would need a
   software compositor (one CPU-allocated buffer per layer + blend
   on the rasterizer). Doable, not done.
2. **fbdev pixel formats** other than 32-bpp BGRA/BGRX are refused.
   RGB565 panels still exist on cost-sensitive SoCs; the swizzle
   path would need a separate inner loop.
3. **`DrmDumbSink` picks the first connected connector** and its
   currently-bound CRTC. Multi-output panels, choosing by name
   (`HDMI-A-1`), or explicit mode selection aren't yet exposed.
   The sink-spec syntax has room — `drm-dumb:/dev/dri/card0,connector=HDMI-A-1,mode=1920x1080@60`
   would parse cleanly when the use case shows up.
4. **Input coverage is partial.** `SoftwareSeat` (see *Input* above)
   handles pointer, keyboard, key-repeat, and multi-touch via
   libinput. Not yet wired: gesture events (`LIBINPUT_EVENT_GESTURE_*`),
   switch events, tablet/pen, hot keymap reload, libseat session
   integration (devices are opened directly under the calling
   process's credentials).
5. **fbdev has no vsync**. `FBIO_WAITFORVSYNC` is broadly broken
   across drivers and not standardized; the sink doesn't expose it.
   For real vsync on a CPU-only SoC, use `drm-dumb` instead.
6. **`DrmDumbSink` refresh from `vrefresh`** is integer Hz; modes
   that report 59.94 round to 60 and Flutter's deadline drifts
   ~one frame per 17 minutes. If drift matters, switch to
   `mode.clock * 1000 / (htotal * vtotal)`.
7. **BE host (big-endian)** isn't tested. `pixel_swizzle.h` keeps a
   correct branch for DRM XRGB (byte order is endian-invariant per
   drm_fourcc.h), but fbdev with `red.offset=16` lands at memory
   `[X,R,G,B]` on BE rather than `[B,G,R,X]` — `FbDevSink` would
   need a dedicated helper. Not implemented since all shipping
   targets are LE; flagged for the day a BE target appears.
