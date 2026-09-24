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
| `PV_BENCH_LAYERS` | `1` | layers per submit (1..3) through `ihs_pv_submit_layers`: the buffer whole, then cropped, placed and mirrored, then cropped, placed and turned a quarter |

`PV_BENCH_NO_PV=1` is the control: same Flutter content, no layer. The difference
between the two runs is what the layer costs.

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
