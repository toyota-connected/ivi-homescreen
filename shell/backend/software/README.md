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

`NoneSink`, `MemorySink`, and `FileSink` are always compiled in.

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
4. **No input plumbing.** For CI, you can drive
   `FlutterEngineSendPointerEvent` via a test helper directly. For
   device targets, evdev / libinput integration is a separate effort
   — the `DrmBackend` already has the integration via `DrmSeat` and
   most of that code transfers.
5. **fbdev has no vsync**. `FBIO_WAITFORVSYNC` is broadly broken
   across drivers and not standardized; the sink doesn't expose it.
   For real vsync on a CPU-only SoC, use `drm-dumb` instead.
6. **`DrmDumbSink` refresh from `vrefresh`** is integer Hz; modes
   that report 59.94 round to 60 and Flutter's deadline drifts
   ~one frame per 17 minutes. If drift matters, switch to
   `mode.clock * 1000 / (htotal * vtotal)`.
