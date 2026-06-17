# scroll_bench

A deterministic, self-driving **list-scroll fps benchmark** for the
ivi-homescreen backends (software / GL / Vulkan).

It renders a long list of "information" rows (icon + title + two-line detail +
trailing value + divider) and, by default, scrolls it continuously at a fixed
velocity — so frame cadence can be profiled with **no input injection** and
every run renders identical pixels.

## What it exercises

* A realistic `ListView.builder` info list (fixed `itemExtent`, 2000 rows).
* Per-row content derived purely from the index → reproducible, no per-frame
  randomness.
* A built-in `ScrollController` auto-scroll that animates top↔bottom at a
  constant velocity (linear curve) and loops. Tap the app-bar play/pause to
  toggle; manual touch/wheel scrolling also works.

This is the realistic counterpart to GPU/blur-heavy apps (e.g. wonderous): a
plain scrollable list is the common IVI list-view workload, so its fps is the
number that matters for that use case.

## Compile-time tunables (`--dart-define`)

| Define | Default | Meaning |
| --- | --- | --- |
| `ITEMS` | `2000` | number of rows |
| `VELOCITY` | `600` | auto-scroll speed, logical px/s |
| `AUTOSCROLL` | `true` | set `false` for a manual (touch-driven) run |

Raise `VELOCITY` / `ITEMS` (and/or drive a higher-res mode) to push past the
vsync cap and find the software-raster ceiling.

## Build (arm64 release AOT) via emb_cli

```sh
source <workspace>/setup_env.sh
emb bundle --build --arch arm64 --mode release \
    -a test/integration/scroll_bench \
    -o test/integration/scroll_bench/bundle-arm64
```

Drop `--arch arm64` for a host (x64) bundle. The result is a homescreen bundle
(`lib/libapp.so` AOT, `data/flutter_assets`, engine + `icudtl.dat`, no
`kernel_blob.bin`).

## Run + profile

The software backend needs DRM master, so stop the compositor first:

```sh
systemctl stop weston            # free /dev/dri/cardN
IVI_SW_PROFILE=1 homescreen -b /path/to/scroll_bench/bundle-arm64 -f
systemctl start weston           # restore afterwards
```

`IVI_SW_PROFILE=1` enables the software backend's `FrameProfile`, which logs
every 60 frames over the drm-cxx `DrmDumbSink` present path:

```
[SoftwareBackend] profile (n=60): fps=59.00 mean_interval=16949us max_interval=17123us \
  present_failures=0 buckets[60Hz/30Hz/20Hz/slow/idle]=56/4/0/0/0
```

Use `IVI_WL_PROFILE` (wayland-egl) or `IVI_VK_PROFILE` (wayland-vulkan /
drm-kms-vulkan) for the same cadence report on the other backends.

## Reference result

i.MX93 (`imx93-11x11-lpddr4x-frdm`), software backend, 1280×720@60, default
tunables: **vsync-locked at ~59 fps** (session 60.03 fps, 90.1% of frames in the
60 Hz bucket, 0 slow frames, 0 present failures).
