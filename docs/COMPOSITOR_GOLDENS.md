# Compositor golden-frame tests

Pixel-diff regression gates for the compositor path. Two layers of coverage:

1. **Single-layer parity** — running the same Flutter app under `BUILD_COMPOSITOR=ON` should produce the same image as under `BUILD_COMPOSITOR=OFF`. Captured by `compositor_headless_golden-test`. This gate flips the moment a compositor change subtly perturbs a Flutter-only frame.
2. **Multi-layer interleave** — Flutter content above and below a `PlatformViewSurface`, expected pixel patterns committed as goldens. Requires a Flutter bundle that emits the platform view; see [Bundle requirements](#bundle-requirements).

Both run against software rasterizers so they're CI-friendly.

## Prerequisites

### Headless software backend

`BUILD_BACKEND_HEADLESS_SOFTWARE` uses Flutter's `kSoftware` CPU renderer — no EGL or GPU required. No additional packages are needed beyond a standard build environment.

### lavapipe (headless Vulkan)

Mesa's software Vulkan ICD. Used to drive `WaylandVulkanBackend` without GPU hardware. Already shipped with `mesa-vulkan-drivers` on most distros — verify with:

```bash
ls /usr/share/vulkan/icd.d/lvp_icd.*.json
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json vulkaninfo --summary
```

Driver name should report `llvmpipe`.

### Bundle requirements

`UNIT_TEST_APP_BUNDLE` must point to a built Flutter bundle. For the parity test any bundle works (it just renders Flutter UI).

For multi-layer goldens the bundle must construct a `PlatformViewSurface` with one of the registered view types (`layer_playground_view` or `nav_render_view`). Minimal example:

```dart
class GoldenScene extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    return Stack(children: [
      Container(color: Colors.green),                       // layer 0
      const SizedBox(
        width: 400, height: 300,
        child: PlatformViewSurface(
          viewType: 'layer_playground_view',
          controller: ...,                                  // layer 1
          gestureRecognizers: <Factory<...>>{},
          hitTestBehavior: PlatformViewHitTestBehavior.opaque,
        ),
      ),
      Positioned(
        top: 100, left: 100,
        child: Text('above', style: TextStyle(fontSize: 48)),  // layer 2
      ),
    ]);
  }
}
```

## Running the parity gate

Configure with both flags on, point at the bundle:

```bash
cmake -S . -B build-goldens \
    -DBUILD_UNIT_TESTS=ON \
    -DBUILD_BACKEND_HEADLESS_SOFTWARE=ON \
    -DBUILD_COMPOSITOR=ON \
    -DUNIT_TEST_APP_BUNDLE=/path/to/flutter/build/linux/x64/release/bundle \
    -DBUILD_NUMBER=1
cmake --build build-goldens --target homescreen_compositor_headless_golden_ut_test_driver -j$(nproc)
ctest --test-dir build-goldens -R compositor_headless_golden -V
```

First run with no committed golden: regenerate.

```bash
cmake -B build-goldens -DUNIT_TEST_SAVE_GOLDENS=ON .
ctest --test-dir build-goldens -R compositor_headless_golden -V    # writes the golden then fails
# Inspect build-goldens/test/unit_test/test_images_golden/*.tga manually.
cmake -B build-goldens -DUNIT_TEST_SAVE_GOLDENS=OFF .
ctest --test-dir build-goldens -R compositor_headless_golden -V    # now passes if pixels match
```

The compare tolerance is per-channel `<= 2` (driver rounding); see `utils_images_are_equal`.

## Running with sanitizers

```bash
cmake -B build-goldens -DSANITIZE_ADDRESS=ON .
cmake --build build-goldens --target homescreen_compositor_headless_golden_ut_test_driver -j$(nproc)
ctest --test-dir build-goldens -R compositor_headless_golden
```

`-DSANITIZE_THREAD=ON` for TSan.

## Vulkan goldens with lavapipe

The `WaylandVulkanBackend` requires a real Wayland compositor (it uses `VK_KHR_wayland_surface` + a swapchain). Lavapipe alone isn't enough — pair it with weston-headless:

```bash
weston --backend=headless-backend.so --width=1920 --height=1080 &
WAYLAND_DISPLAY=wayland-1 \
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
    homescreen -b "$BUNDLE" --window-type=BG
```

There is no native headless-Vulkan backend in this repo today (the existing `HeadlessBackend` is Software-only). Adding one would mean a fourth backend implementation along the lines of `WaylandVulkanBackend` but using `VK_EXT_headless_surface` + a memory-image swapchain — substantial, not on this branch. Until then, Vulkan goldens go through the weston-headless pairing above.

## What's deferred

- **Native headless Vulkan backend.** See above.
- **Multi-layer goldens.** Need a Dart bundle that emits a `PlatformViewLayer`; the renderer will then exercise `WaylandEglBackend::PresentLayers`'s general path and the new GL-texture composite step from the plugin migrations.

When those land, the additional test cases drop into `compositor_headless_golden-test/` next to the existing parity test.
