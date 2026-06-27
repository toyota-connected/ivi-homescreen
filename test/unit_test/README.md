# How to run unit test
## Run test one by one
### build
```bash
$ mkdir build && cd build
build$ cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local -DBUILD_UNIT_TESTS=1 -DCMAKE_BUILD_TYPE=Debug
```

### run all test
```bash
build$ ctest
```

### run test per test case
```bash
build$ ./test/unit_test/template-test-case/templateTest
```

## Run all test with measuring coverage
### build and run test
```bash
build$ cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local -DBUILD_UNIT_TESTS=1 -DCOVERAGE=1 -DCMAKE_BUILD_TYPE=Debug
build$ ./run-ut-coverage.sh
```

### Open html files.
(ex: use firefox)

```bash
build$ firefox lcovHtml/index.html
```

# Writing tests
## Flutter App 
For unit tests that generate/compare images, a flutter app bundle must be provided via the UNIT_TEST_APP_BUNDLE option.  

## Image Comparisons
To test the output of rendering functions, it may be necessary to compare the test output to a previously taken image that is known to be correct.  To obtain these images, run cmake with the options BUILD_UNIT_TEST and UNIT_TEST_SAVE_GOLDENS set to true.  In the implemention of the unit test, when this flag is set, the output image should just be saved to the build/test/unit_test/test-images-golden directory.  Manually inspect the output images to verify they are correct.  It is also recommended to ensure that the test fails afterwards, in case this setting is ever set inadvertently.  When UNIT_TEST_SAVE_GOLDENS is set to false, then the test shall create the image and then compare to the one in this directory.

# Compositor tests

Three backend-agnostic test drivers cover the compositor primitives:

- `backing_store_pool-test` — `BackingStorePool<T>` acquire/release/cap/flush.
- `present_layer_sequencer-test` — Wayland subsurface Z-order reconciliation (uses an injected placer hook; no real Wayland needed).
- `compositor_registry-test` — polymorphic `Backend::{Register,Unregister,Resize}CompositorSurface` dispatch and `ICompositorSurface::OnResize`. Only compiled when `-DBUILD_COMPOSITOR=ON`.

Run with sanitizers:

```bash
build$ cmake .. -DBUILD_UNIT_TESTS=1 -DBUILD_COMPOSITOR=1 -DSANITIZE_ADDRESS=ON
build$ ctest -V
```

or `-DSANITIZE_THREAD=ON` for TSan. All three drivers are sanitizer-clean.

## Deferred: golden-frame tests

Full golden-frame tests for the compositor path (single-layer parity vs. the non-compositor baseline, two-layer interleave on EGL and Vulkan, Filament plugin composition) require:

- A Flutter bundle that produces a `PlatformViewLayer` in its frame graph.
- A software or Vulkan context (e.g. `lavapipe`) to run without a display.

The groundwork — `ImageType`, `utils_write_targa`, `utils_images_are_equal`, `UNIT_TEST_SAVE_GOLDENS` — is in place, but the compositor tests aren't written yet. They land when the offscreen-compositor harness does.

## Live integration script

`test/compositor_integration.sh` runs the compositor build against a real Wayland compositor (weston) and scans the log for errors. It is not run from ctest; invoke it manually after installing the binary. Optional video capture via `wf-recorder` + `ffprobe` verifies frame pacing.
