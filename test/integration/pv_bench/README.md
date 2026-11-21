# pv_bench

What a platform-view layer costs the compositor path.

`scroll_bench` measures Flutter's own raster. This measures what the shell adds
when the scene is not Flutter's alone: one producer-owned dma-buf layer,
submitted at the display rate, with Flutter content over it.

The producer is deliberately dumb -- a CPU-filled sweeping band out of a
three-buffer GBM ring. A GL or Vulkan producer would fold its own renderer into
every number, and the question here is what the shell does with a layer, not how
fast a plugin can draw one.

## What runs where

`ihs_pv_negotiate` is asked for `TEXTURE_DMABUF_IMPORT`, with `SOFTWARE_SHM` left
in the mask as a floor. On the Vulkan backend the frame is imported as a VkImage
and composited; on `drm-kms-egl` it is imported into a GL texture. `DRM_PLANE` is
not asked for, even though the EGL backend offers it -- taking it there would
measure direct scanout on one backend and composition on the other, under one
name.

`PV_BENCH_DRM_PLANE=1` adds `DRM_PLANE` to the mask *and* names it in
`IhsPvRequirements::preferred_kind` (ABI 1.17). The preference is the part that
matters: the default order ranks `TEXTURE_DMABUF_IMPORT` above `DRM_PLANE` and
`choose_kind` takes the first match, so adding the kind to the mask alone
changes nothing (#673). Before 1.17 the only way through was to request
`DRM_PLANE` alone, which left the run with no fallback.

The producer logs the granted kind at startup. If it does not say
`kind=0x1`, the run is not measuring the import path and the numbers are not
comparable to one that is.

Once a second it logs its submit rate beside the rate at which the shell
reported its frames on screen (`IhsPvCallbacks::presented`), and the share of
those that went from the producer's buffer straight to a plane
(`IHS_PV_PRESENTED_ZERO_COPY`). Presented below submitted means frames were
replaced before they were shown; 0 presented means a shell older than ABI
1.12.

## Build

The producer is built out of tree, against a configured shell build:

```sh
cmake -G Ninja -B <build> -S producer -DCMAKE_BUILD_TYPE=Release \
      -DIHS_BUILD_DIR=<shell build dir> \
      [-DCMAKE_TOOLCHAIN_FILE=<cross toolchain>]
cmake --build <build>
```

Then the app bundle, and drop the producer beside the engine:

```sh
source <workspace>/setup_env.sh
emb bundle --build --arch arm64 --mode release -a . -o bundle-arm64
cp <build>/libpv_bench_producer.so bundle-arm64/lib/
```

## Run

The app loads the producer over FFI and registers its factory itself, so the
shell needs no plugin support and no rebuild.

```sh
PV_BENCH_PRODUCER=$PWD/lib/libpv_bench_producer.so \
BENCH_SECONDS=20 SUMMARY_FILE=/tmp/pv.txt IHS_LOG_RING_CAPACITY=8192 \
LD_LIBRARY_PATH=$PWD/lib \
  ./homescreen -b $PWD --backend drm-kms-vulkan \
      --drm-compositor planes --engine-arg=--enable-impeller -f
```

`--drm-compositor planes` selects the compositor, `gl` the root surface (see
\#594). `--engine-arg=--enable-impeller` picks the renderer. `IHS_LOG_RING_CAPACITY`
matters: the shared log ring drops records silently at its 256-slot default and
the startup flood overruns it every run.

Leave at least a couple of seconds between runs. Starting the next one before the
previous process has released DRM master gives a run that produces no report.

| Variable | Default | Meaning |
| --- | --- | --- |
| `PV_BENCH_PRODUCER` | `lib/libpv_bench_producer.so` | producer to load |
| `PV_BENCH_NO_PV` | *(unset)* | run the Flutter content with no platform view |
| `BENCH_SECONDS` | `20` | run length, then report and exit |
| `TIMINGS_FILE` | *(unset)* | per-frame timing CSV |
| `SUMMARY_FILE` | *(unset)* | the summary, written to a file as well as stdout |
| `PV_BENCH_DRM_NODE` | *(unset)* | allocate on this node instead of probing |
| `PV_BENCH_PAD_ROWS` | tile-rounded | rows of allocation headroom; 0 reproduces \#598 |
| `PV_BENCH_TILED` | *(unset)* | `1`: take the backend's format order, which can grant a tiled buffer; by default a linear one is asked for first, since the CPU fill scrambles tiled content (\#872) |
| `PV_BENCH_LAYERS` | `1` | layers per submit (1..3) through `ihs_pv_submit_layers`: the buffer whole, then cropped, placed and mirrored, then cropped, placed and turned a quarter |
| `PV_BENCH_DRM_PLANE` | *(unset)* | prefer `DRM_PLANE`, keeping import and shm as fallbacks: measures direct scanout rather than composition. Offered only by `drm-kms-egl`; the run logs whether a plane was actually granted, so a fallback cannot be mistaken for a plane measurement |
| `PV_BENCH_SHM` | *(unset)* | prefer `SOFTWARE_SHM` and fill the host's buffers (ABI 1.19) instead of the producer's own ring: the CPU floor, composited. One layer only. The per-second log's release timeouts should stay 0 |
| `PV_BENCH_SCALE` | `1` | the app wraps its view in `Transform.scale` by this factor, about its center |
| `PV_BENCH_SCALE_ANIM` | *(unset)* | `1`: the app wraps its view in a `ScaleTransition` between 0.4 and 1.0 instead, two seconds each way |
| `PV_BENCH_CLIP` | *(unset)* | `1`: the app wraps its view in a `ClipRect` that keeps the middle of the screen, half its width and height |
| `PV_BENCH_OPACITY` | `1` | the app wraps its view in `Opacity` at this value |
| `PV_BENCH_RADIUS` | `0` | the app wraps its view in a `ClipRRect` with corners of this radius, in logical pixels |
| `PV_BENCH_ROTATE` | `0` | the app wraps its view in `Transform.rotate` by this many degrees about its center |
| `PV_BENCH_ROTATE_ANIM` | *(unset)* | `1`: the app wraps its view in a `RotationTransition` instead, one turn every eight seconds |
| `PV_BENCH_FADE` | *(unset)* | `1`: the app wraps its view in a `FadeTransition` instead: up from 0.2 to 1.0 over two seconds, a second at 1.0, and back down over two |

`PV_BENCH_NO_PV=1` is the control: same Flutter content, no layer. The difference
between the two runs is what the layer costs.

`PV_BENCH_SCALE` and `PV_BENCH_SCALE_ANIM` are read by the app, not the shell.
The shell has no scale setting. In a real app the scale is whatever transform
the Dart code puts around each platform view, and each view can have its own.
pv_bench has one view and no UI, so these two pick the transform it wraps that
view in, and one bundle can run scaled or not without a rebuild.

The shell sees the transform the way it sees any app's: in the mutation list
the engine sends with the view's layer each frame, from which it works out that
view's drawn size.

They are for checking placement rather than for timing. At
`PV_BENCH_SCALE=0.5` on a full-screen view, the view should cover half the
output's width and height, centered. A view drawn full size from that
rectangle's top-left corner, running off the bottom right, is the bug in
\#865, fixed for the texture and plane paths in \#871. With
`PV_BENCH_SCALE_ANIM=1` and a plane path, the plane's destination rect should
move while its source rect stays the size of the producer's buffer: the
producer is not resized while the scale animates.

`PV_BENCH_CLIP=1` is the same kind of check for a clip, also read by the app.
The view keeps its full-screen rect, and only the middle of it, half the
output's width and height and centered, should show. The whole view showing
is the bug in \#863. On a plane path the plane's destination rect should be
that middle rect, and its source rect the matching part of the buffer.

`PV_BENCH_OPACITY` and `PV_BENCH_FADE` are the same kind of check for an
opacity, also read by the app. The producer's green and blue are fixed at 0x40
and 0x80 and the app's background is black, so at `PV_BENCH_OPACITY=0.5` the
view should read about 0x20 green and 0x40 blue wherever the overlay is not
over it. A view at full strength is the bug in \#864. On a plane path the
plane's `alpha` property should be half its range. `PV_BENCH_FADE=1` holds
1.0 for a second each cycle, where Flutter drops the opacity, so a plane path
also has to go back to opaque: its `alpha` should read the top of its range
for that second.

`PV_BENCH_RADIUS` is the same kind of check for a rounded clip, also read by
the app. On a full-screen view the output's corners should show the app's
black background, cut along a curve of that radius times the pixel ratio, and
everything else the view. Square corners are the bug in \#867. No plane has
rounded corners, so on a plane path the view should not get one: the frame is
composited instead.

`PV_BENCH_ROTATE` and `PV_BENCH_ROTATE_ANIM` are the same kind of check for a
rotation, also read by the app. At `PV_BENCH_ROTATE=30` the view should be
turned 30 degrees clockwise about the output's center, with the app's black
background in the corners it uncovers. An upright view, or one shifted toward
the corner of its bounding box, is the bug in \#866. No plane turns by an
arbitrary angle, so on a plane path the frame is composited instead. They go
inside the clip and the rounded clip, so with `PV_BENCH_CLIP` or
`PV_BENCH_RADIUS` the turned view is cut by an upright clip.

The app reports build/raster/total percentiles and exits. The producer reports
submits per second and mean release-fence wait every second -- check that first.
A producer that is not pinned at the display rate makes every downstream number
a description of a starved producer rather than of the shell.

## Comparing

Six runs, each config with the layer and without:

| Run | What it shows |
| --- | --- |
| Impeller + compositor | the layer is composited; what that costs |
| Skia + compositor | the same scene through the other rasterizer |
| Impeller + root surface | no compositor, so the layer has nowhere to go |

The third is not a fair third data point -- the layer is not on screen -- but it
is the number people quote when they say the compositor is expensive, so it is
worth having measured rather than assumed.

A short run does not settle. Measuring a regression here at 6 s read 62 fps
against 42.7 over 20 s; use 20.
