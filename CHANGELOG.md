# Changelog

## v3.0.0

The v2 shell drove one Wayland display through one of two backends chosen at
build time. v3 is a display-target layer: eight backends, any combination
compiled in, the active one resolved per view at runtime.

### Backends

-no longer mutually exclusive. every `BUILD_BACKEND_*` may be ON together and
 `BackendRegistry` resolves the active one per view, so one process can drive
 heterogeneous targets
-new: DRM/KMS on EGL and on Vulkan, software (CPU, no GPU or display needed),
 Wayland leased-DRM, headless EGL, headless Vulkan
-v2 had wayland-egl and wayland-vulkan, and a Wayland DRM backend that was off
 by default

### Compositing and platform views

-`BUILD_COMPOSITOR` adds the FlutterCompositor backing-store path, so
 platform-view layers interleave with Flutter content instead of being drawn
 under or over all of it
-platform views reach a KMS plane on the DRM backends and a wl_subsurface on
 Wayland, with dma-buf import where the driver allows it
-`BUILD_COMPOSITOR_DMABUF_EXPORT` exports a view's buffer to a consumer
-`BUILD_HUD` draws a Dear ImGui overlay of per-frame cost, behind `IVI_HUD`

### Shells

-xdg, agl, ivi and simple, selected with `--shell` or resolved automatically
-the AGL shell carries the window-management facet: background and panel roles,
 the activation area, and the `ready` handshake

### New subsystems

-accessibility semantics tree, with an AccessKit adapter
-crash handler; the DSN comes from `SENTRY_DSN` in the environment at runtime
-output manager: per-view output binding, and the transitions a view sees when
 its output appears, moves, is lost or is reconfigured
-input, logging, profiling (frame cost and motion-to-photon), vsync providers,
 watchdog, and an OSGi multi-bundle host
-`BUILD_MCP` exposes a control surface; `BUILD_FUZZERS` builds
 coverage-guided targets

### Build

-the Wayland protocol bindings are generated at build time by
 wayland-cxx-scanner, which runs on the build host
-`ENABLE_STATIC_LINK` links the C++ runtime statically, verified after the link
 rather than assumed
-sanitizer builds for the unit tests, and the suite runs under address, thread
 and undefined

### Testing

-77 unit test directories and 14 integration applications
-a DRM harness that runs against vkms, and a headless Wayland smoke that
 renders a frame and checks it is not blank

### Packaging notes

-the project version is `3.0.0`. `BUILD_NUMBER` is separate and identifies the
 CI run that produced a binary, where before the two were folded together and
 a 3.0 branch reported `1.0.<run id>`
-`--backend` selects among whatever was compiled in, so a package can ship one
 binary for several targets
