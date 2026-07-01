# multi_view_test — multi-monitor single-engine integration test

Validates that **one** Flutter engine can drive **multiple** monitors — one
bundle → one engine → N views → N outputs. Run it to check your platform
supports single-engine multi-monitor.

## What it proves

- The app is multi-view (`runWidget` + a `View` per `FlutterView`).
- Every view shows a shared `tick` counter that lives in **one isolate**. If a
  single engine drives all monitors, every screen shows the **same tick value in
  lockstep**. Divergent values would mean separate engines (that would be case
  A, and the test fails).
- Each view shows its own `VIEW n` label and a distinct background colour, so
  the outputs are visibly distinct views.

## Build the bundle

Like the other integration apps (see `../scroll_bench/README.md`), built with
the workspace Flutter toolchain / `emb_cli`:

```sh
source <workspace>/setup_env.sh
emb bundle --build --mode release -a test/integration/multi_view_test \
    -o test/integration/multi_view_test/bundle
```

Drop `--mode release` for a debug (JIT) bundle; add `--arch arm64` to
cross-build. The app uses Flutter's multi-view API — build against an engine
that supports it.

## Run

Single engine to two outputs on one card. The harness writes a config
(one `[[view]]` with two `[[view.output]]`) into a throwaway copy of the bundle.

Real hardware (two monitors on one card):

```sh
HOMESCREEN=cmake-build-debug-clang/shell/homescreen \
BUNDLE=test/integration/multi_view_test/bundle \
DRM_DEVICE=/dev/dri/card1 CONNECTOR_A=HDMI-A-1 CONNECTOR_B=DP-1 \
COUNT_FLIPS=1 CAPTURE=1 \
  test/multi_view_single_engine.sh
```

Portable / CI (no second monitor) via virtual KMS:

```sh
sudo third_party/drm-cxx/scripts/vkms_dual.sh up
HOMESCREEN=... BUNDLE=... DRM_DEVICE=/dev/dri/cardN \
CONNECTOR_A=<vkms-conn-1> CONNECTOR_B=<vkms-conn-2> SOFTWARE_RENDER=1 \
  test/multi_view_single_engine.sh
```

The harness asserts: both connectors came up under one engine, no second
(discrete) engine started, no error/critical logs, and — with `COUNT_FLIPS=1` —
that both CRTCs are presenting. `CAPTURE=1` dumps a PNG per output
(`IVI_DRM_CAPTURE` + `SIGUSR1`) for pixel evidence.

## Requirements

The single-engine multi-output path lives under `BUILD_COMPOSITOR=ON`; build the
shell with that flag. With it off, the DRM backend renders a single output only
and the second view will not appear.
