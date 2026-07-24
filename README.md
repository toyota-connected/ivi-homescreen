# ivi-homescreen

Flutter Linux CPP Embedder

[![Documentation Status](https://readthedocs.org/projects/ivi-homescreen/badge/?version=latest)](https://ivi-homescreen.readthedocs.io/en/latest/?badge=latest)

#### Discord Server https://discord.gg/V5uWD9fvws

## Highlights

* Desktop Plugin Registry
    * Flutter Pigeon CPP compatible
    * Plugins modeled after Window CPP
    * Plugins enabled/disabled via CMake
    * Firestore first party compatible
* Desktop Texture Registry
    * Camera first party compatible
    * Video Player first party compatible
* Platform View Framework
    * AndroidView widget compatible
* Backend Support (any subset builds into one binary; the active backend is
  selected at runtime — process-wide via `--backend`, or per view via
  `[view.backend]`)
    * Wayland EGL
    * Wayland Vulkan
    * DRM/KMS EGL (direct-to-display, no compositor)
    * DRM/KMS Vulkan (zero-copy dma-buf scanout)
    * Software (CPU renderer, no GPU or display-server dependency)
    * Leased DRM, EGL / Vulkan / software (drm-lease-v1: own a connector leased
      from a compositor while it keeps the rest of the card)
* Multi-display
    * Multiple views across multiple outputs from one process
    * Per-view output binding by connector / `wl_output` name (`[view.output]`)
    * Combined-space pointer routing, per-display touch, and a single cursor
      that follows the pointer across displays
* Same source code runs on Desktop and embedded Linux image
    * Ubuntu 18+
    * Fedora 33+
    * Yocto Dunfell/Kirkstone/Scarthgap

## Plugins

ivi-homescreen plugins are located at https://github.com/toyota-connected/ivi-homescreen-plugins

There are two ways to reference this repo:

1. Clone plugins repo to root of ivi-homescreen folder
2. Set PLUGIN_DIR to repo path. -DPLUGIN_DIR=<my path>

## Logging

Logging level support

* trace
* debug
* info
* warn
* error
* critical
* off

If environmental variable IHS_LOG_LEVEL is set, it will override the default logging level of info. The logging level can also be set via the command line argument --log-level <level>.

### DLT logging

To test DLT logging on desktop use the following

Ubuntu packages

    sudo apt-get install libdlt-dev dlt-viewer dlt-daemon dlt-tools

Fedora packages

    sudo dnf install dlt-libs-devel dlt-daemon dlt-tools

### Logging with DLT

Start new terminal

    dlt-daemon

#### View DLT log output in a terminal

Start new terminal

    dlt-receive -a localhost

## Sanitizer Support

You can enable the sanitizers with SANITIZE_ADDRESS, SANITIZE_MEMORY, SANITIZE_THREAD or SANITIZE_UNDEFINED options in
your CMake configuration. You can do this by passing e.g. -DSANITIZE_ADDRESS=On on your command line.

If sanitizers are supported by your compiler, the specified targets will be built with sanitizer support. If your
compiler has no sanitizing capabilities you'll get a warning but CMake will continue processing and sanitizing will
simply just be ignored.

## Backend Support

Any subset of backends can be compiled into a single binary; they are no longer
mutually exclusive. The active backend is resolved at runtime — process-wide via
`--backend`, or per view via `[view.backend] type` in the bundle's config.toml
(CLI overrides config). A single-backend build simply registers one backend and
uses it.

| Backend | Registry key | CMake option | Notes |
|---|---|---|---|
| Wayland EGL | `wayland-egl` | `BUILD_BACKEND_WAYLAND_EGL` | GL renderer on a Wayland compositor (default ON) |
| Wayland Vulkan | `wayland-vulkan` | `BUILD_BACKEND_WAYLAND_VULKAN` | Vulkan renderer on a Wayland compositor |
| DRM/KMS EGL | `drm-kms-egl` | `BUILD_BACKEND_DRM_KMS_EGL` | Direct-to-display GL on bare KMS (no compositor) |
| DRM/KMS Vulkan | `drm-kms-vulkan` | `BUILD_BACKEND_DRM_KMS_VULKAN` | Zero-copy dma-buf scanout on bare KMS |
| Software | `software` | `BUILD_BACKEND_SOFTWARE` | CPU renderer; no GPU or display server |
| Leased DRM (EGL) | `wayland-leased-drm-egl` | `BUILD_BACKEND_WAYLAND_LEASED_DRM` + `BUILD_BACKEND_DRM_KMS_EGL` | GL on a connector leased from a compositor via drm-lease-v1 |
| Leased DRM (Vulkan) | `wayland-leased-drm-vulkan` | `BUILD_BACKEND_WAYLAND_LEASED_DRM` + `BUILD_BACKEND_DRM_KMS_VULKAN` | Zero-copy dma-buf scanout on a leased connector |
| Leased DRM (software) | `wayland-leased-drm-software` | `BUILD_BACKEND_WAYLAND_LEASED_DRM` + `BUILD_BACKEND_SOFTWARE` | CPU renderer on a leased connector |

With no explicit selection, the resolver is environment-aware: a live Wayland
session picks `wayland-egl`, otherwise `drm-kms-egl`.

The `wayland-leased-drm-*` backends acquire a connector from a running Wayland
compositor rather than opening a card, so one process can own a panel while the
compositor owns the rest of the GPU. They are never selected implicitly, and a
leased key is refused rather than substituted if unavailable — falling back to an
unleased backend would grab hardware the operator did not ask for. The bare
family name `wayland-leased-drm` picks the first available tier. Note that a
compositor implementing drm-lease-v1 is necessary but **not** sufficient: the
wlroots family only offers connectors flagged non-desktop in EDID, so an ordinary
panel is not leasable without intervention. See
[shell/backend/wayland_leased_drm/README.md](shell/backend/wayland_leased_drm/README.md).

Running a Vulkan backend requires an engine build that supports Vulkan.

## Bundle File Override Logic

If an override file is not present, it gets loaded from a default location.

### Optional override files

#### icudtl.dat

Bundle Override

    {bundle path}/data/icudtl.dat

Yocto Default

    /usr/share/flutter/icudtl.dat

Desktop Default

    /usr/local/share/flutter/icudtl.dat

#### libflutter_engine.so

Bundle Override

    {bundle path}/lib/libflutter_engine.so

Yocto/Desktop Default - https://tldp.org/HOWTO/Program-Library-HOWTO/shared-libraries.html

## Command Line Options and Configuration

All CLI flags, the full `config.toml` reference, the schema walkthrough, the parameter loading order, and multi-display examples now live with the configuration subsystem's documentation:
[`shell/configuration/README.md`](shell/configuration/README.md).

That document holds the generated CLI and configuration-reference tables, kept in sync with the parser by [`scripts/gen_config_reference.py`](scripts/gen_config_reference.py) (do not edit `CLI-REFERENCE` sections by hand).

A bundle (`-b`) directory has this structure:

```
  Flutter Application (bundle folder)
    data/flutter_assets
    data/icudtl.dat (optional - overrides system path)
    lib/libapp.so
    lib/libflutter_engine.so (optional - overrides system path)
```

See the [configuration docs](shell/configuration/README.md) for the complete option list, `config.toml` schema, and examples.

## CMake Build flags

`ENABLE_XDG_CLIENT` - Enable XDG Client. Defaults to ON

`ENABLE_AGL_SHELL_CLIENT` - Enable AGL Client. Defaults to OFF

`ENABLE_IVI_SHELL_CLIENT` - Enable ivi-shell Client. Defaults to OFF

`ENABLE_DRM_LEASE_CLIENT` - Enable drm lease Client. Defaults to OFF

`ENABLE_LTO` - Enable Link Time Optimization. Defaults to OFF

`ENABLE_DLT` - Enable DLT logging. Defaults to OFF

`BUILD_BACKEND_WAYLAND_EGL` - Build Backend for EGL. Defaults to ON

`BUILD_EGL_TRANSPARENCY` - Build with EGL Transparency Enabled. Defaults to ON

`BUILD_EGL_ENABLE_3D` - Build with EGL Stencil, Depth, and Stencil config Enabled. Defaults to ON

`BUILD_EGL_ENABLE_MULTISAMPLE` - Build with EGL Sample set to 4. Defaults to ON

`BUILD_BACKEND_WAYLAND_VULKAN` - Build Backed for Vulkan. Defaults to OFF

`BUILD_COMPOSITOR` - Enable the `FlutterCompositor` backing-store API so platform-view layers can be interleaved with Flutter UI. See [Platform View Plugins](#platform-view-plugins). Defaults to OFF.

`BUILD_COMPOSITOR_DMABUF_EXPORT` - When the Vulkan backend is active and `BUILD_COMPOSITOR=ON`, export each `VulkanBackingStore`'s memory as a DMA-BUF fd so plugins can import it zero-copy (requires `VK_KHR_external_memory_fd` at runtime; silently falls back to a plain allocation if unavailable). Defaults to OFF.

`DEBUG_PLATFORM_MESSAGES` - Dump Platform Channel Messages. Defaults to OFF

`BUILD_CRASH_HANDLER` - Build Sentry IO Crash Handler Support. Defaults to OFF

`BUILD_DOCS` - Builds Docs. Defaults to OFF

`BUILD_UNIT_TESTS` - Build Unit Tests. Defaults to OFF

`UNIT_TEST_SAVE_GOLDENS` - Update test goldens. Defaults to OFF

`EXE_OUTPUT_NAME` - Set executable output name. Defaults to `homescreen`

`DISABLE_PLUGINS` - Disables all plugins located in the plugins folder. Defaults to OFF

`BUILD_PLUGIN_AUDIOPLAYERS_LINUX` - Include Audioplayers Linux plugin. Defaults to OFF

`BUILD_PLUGIN_CAMERA` - Include Camera plugin. Defaults to OFF

`BUILD_PLUGIN_CLOUD_FIRESTORE` - Plugin Cloud Firestore. Defaults to OFF

`BUILD_PLUGIN_DESKTOP_WINDOW_LINUX` - Includes Desktop Window Linux Plugin. Defaults to OFF

`BUILD_PLUGIN_FILE_SELECTOR` - Include File Selector plugin. Defaults to OFF

`BUILD_PLUGIN_FIREBASE_AUTH` - Plugin Firebase Auth. Defaults to OFF

`BUILD_PLUGIN_FIREBASE_STORAGE` - Plugin Firebase Storage. Defaults to OFF

`BUILD_PLUGIN_GO_ROUTER` - Includes Go Router Plugin. Defaults to ON

`BUILD_PLUGIN_GOOGLE_SIGN_IN` - Include Google Sign In manager. Defaults to OFF

`BUILD_PLUGIN_INTEGRATION_TEST` - Included Flutter Integration Test support. Defaults to OFF

`BUILD_PLUGIN_PDF` - Include PDF plugin. Defaults to OFF

`BUILD_PLUGIN_SECURE_STORAGE` - Includes Flutter Secure Storage. Defaults to OFF

`BUILD_PLUGIN_URL_LAUNCHER` - Includes URL Launcher Plugin. Defaults to OFF

`BUILD_PLUGIN_VIDEO_PLAYER_LINUX` - Include Video Player plugin. Defaults to OFF

`BUILD_PLUGIN_FILAMENT_VIEW` - Include Filament View plugin. Defaults to OFF

`BUILD_PLUGIN_LAYER_PLAYGROUND_VIEW` - Include Layer Playground View plugin. Defaults to OFF

`BUILD_PLUGIN_NAV_RENDER_VIEW` - Include Navigation Render View plugin. Defaults to OFF

`BUILD_PLUGIN_WEBIVEW_FLUTTER_VIEW` - Includes WebView View Plugin. Defaults to OFF

`BUILD_WATCHDOG` - Build Watchdog support. Monitors main and render threads for hangs and aborts on timeout. Defaults to OFF

`BUILD_SYSTEMD_WATCHDOG` - Integrate with systemd watchdog (sd_notify). Requires `BUILD_WATCHDOG=ON` and a systemd-enabled Linux distro. Defaults to OFF

Each `BUILD_BACKEND_*` option gates whether that backend is compiled in; any
subset may be enabled together and the active one is chosen at runtime (see
[Backend Support](#backend-support)).

## Platform View Plugins

When `-DBUILD_COMPOSITOR=ON`, the EGL and Vulkan backends wire the `FlutterCompositor` backing-store API so that a plugin-owned native surface can be interleaved between Flutter-rendered layers — what the Flutter framework calls a `PlatformViewLayer`. Without this flag the engine runs in single-surface mode and platform views fall back to full-screen overlays.

A plugin participates by implementing `ICompositorSurface` (`shell/view/compositor_surface_interface.h`) and registering the instance with `FlutterView` when the Dart widget is created:

```cpp
#include "view/compositor_surface_interface.h"
#include "view/flutter_view.h"

class MyPlatformView : public ICompositorSurface {
 public:
  explicit MyPlatformView(FlutterPlatformViewIdentifier id) : id_(id) {}

  bool OnCreateBackingStore(const FlutterBackingStoreConfig* config,
                            FlutterBackingStore* out) override {
    // Fill `out` with a backing store your plugin renders into. Most
    // plugins delegate to the engine-provided backing store and only
    // override if they need a specific image/format (e.g. DMA-BUF).
    return false;
  }
  bool OnCollectBackingStore(const FlutterBackingStore*) override { return true; }
  bool OnPresent(const FlutterLayer* layer) override {
    // Draw / swap your native surface here. The compositor has already
    // reconciled the Wayland subsurface Z-order before this call.
    return true;
  }
  FlutterPlatformViewIdentifier GetIdentifier() const override { return id_; }
  void OnResize(int32_t w, int32_t h) override { /* re-size native surface */ }

 private:
  FlutterPlatformViewIdentifier id_;
};

// On the flutter/platform_views "create" message:
flutter_view->RegisterCompositorSurface(
    id, std::make_shared<MyPlatformView>(id));

// On "dispose":
flutter_view->UnregisterCompositorSurface(id);
```

`PlatformViewsHandler` calls `UnregisterCompositorSurface` automatically on `dispose` and routes `resize` messages to `ICompositorSurface::OnResize`, so plugins only need to register on create.

Key details:

- `OnPresent` runs on the engine's rasterizer thread. If your plugin renders on its own thread, marshal work there.
- The Vulkan backend hands the engine a `VkImage` in `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`. The compositor records all layout transitions — plugins that *consume* the image via `ICompositorSurface::OnPresent` should be prepared to sample in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` (compositor transitions it to `TRANSFER_SRC_OPTIMAL` during the blit; see `WaylandVulkanBackend::BlitStoreToSwapchain`).
- With `-DBUILD_COMPOSITOR_DMABUF_EXPORT=ON`, a plugin can query `WaylandVulkanBackend::HasDmaBufExport()` to know whether the store exposes a DMA-BUF fd for zero-copy import into EGL/KMS/Filament.
- On pure GLES2 drivers, the EGL compositor falls back to a textured-quad program instead of `glBlitFramebuffer`; no plugin action required.

Compositor mode is opt-in. The default (`BUILD_COMPOSITOR=OFF`) build is unchanged, so existing plugins that draw via a legacy `wl_subsurface` path keep working as before.

## x86_64 Desktop development notes

### NVidia GL errors

Running EGL backend on a Lenovo Thinkpad with NVidia drivers may generate many GL runtime errors.
This should resolve it:

```
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
```

## Ubuntu 16-18

### Logging in

Log out if logged in Login screen
Click on username field
Right-click on the gear icon below username field, and select "Ubuntu on Wayland"
Enter password and login

## Ubuntu 20+ / Fedora 33+

Defaults to Wayland, no need to do anything special

## Build steps

### Required Packages

    sudo add-apt-repository ppa:kisak/kisak-mesa
    sudo apt-get update -y
    sudo apt-get -y install libwayland-dev wayland-protocols \
    mesa-common-dev libegl1-mesa-dev libgles2-mesa-dev mesa-utils \
    libxkbcommon-dev

### Optional Packages

    # To build doxygen documentation
    sudo apt-get -y install doxygen

### GCC/libstdc++ Build

Without plugins

    git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git
    mkdir build && cd build
    cmake ../ivi-homescreen -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
    make install -j

With plugins

    git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git
    git clone https://github.com/toyota-connected/ivi-homescreen-plugins.git
    mkdir build && cd build
    cmake ../ivi-homescreen -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local -DPLUGINS_DIR=`pwd`/ivi-homescreen-plugins
    make install -j

### Clang/libc++ Build

Without plugins

    git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git
    mkdir build && cd build
    CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake ../ivi-homescreen -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
    make install -j

With plugins

    git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git
    git clone https://github.com/toyota-connected/ivi-homescreen-plugins.git
    mkdir build && cd build
    CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake ../ivi-homescreen -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local -DPLUGINS_DIR=`pwd`/ivi-homescreen-plugins
    make install -j

#### Clang Toolchain Setup

    wget https://apt.llvm.org/llvm.sh
    chmod +x llvm.sh
    sudo ./llvm.sh 14
    sudo apt-get install -y libc++-14-dev libc++abi-14-dev libunwind-dev

## CI Example

    https://github.com/toyota-connected-na/ivi-homescreen/blob/main/.github/workflows/ivi-homescreen-linux.yml

## Debian Package

    make package -j
    sudo apt install ./ivi-homescreen-1.0.0-Release-beta-Linux-x86_64.deb

## Flutter Application

### Running an app

Release Bundle Folder layout

```
.desktop-homescreen/
├── data
│ ├── flutter_assets
│ │   └── ... 
│ └── icudtl.dat
├── default_config.json (optional)
└── lib
    ├── libapp.so
    └── libflutter_engine.so
```

Running the bundle above would be

```
homescreen --b=`pwd`/.desktop-homescreen --w=1024 --h=768
```

## workspace-automation provides a flutter workspace setup tool

https://github.com/meta-flutter/workspace-automation

Example usage to run gallery application on Linux desktop

Run once

```
git clone https://github.com/meta-flutter/workspace-automation
cd workspace_automation
sudo ./flutter_workspace.py
```

Run for each development session, or new terminal window opened

```
source ./setup_env.sh
cd app/gallery
flutter run -d desktop-homescreen
```

flutter_workspace.py installs runtime packages, patches source files, compiles projects, etc.

_Note: `sudo` is required to install runtime packages_

## CMAKE dependency paths

Path prefix used to determine required files is determined at build.

For desktop `CMAKE_INSTALL_PREFIX` defaults to `/usr/local`
For target Yocto builds `CMAKE_INSTALL_PREFIX` defaults to `/usr`

## Watchdog

The watchdog monitors the main thread and Flutter render thread for hangs. If either thread fails to check in within the timeout window (default 5 seconds), the process calls `abort()` to generate a core dump — or triggers `sd_notify(WATCHDOG=trigger)` when built with systemd support.

### CMake Variables

To enable watchdog support:

    -DBUILD_WATCHDOG=ON

To additionally integrate with the systemd service watchdog:

    -DBUILD_WATCHDOG=ON -DBUILD_SYSTEMD_WATCHDOG=ON

With systemd integration enabled, the embedder reads the `WatchdogSec=` interval from the service unit, sends `READY=1` on startup, `WATCHDOG=1` each ping, and `STOPPING=1` on clean shutdown.

### Watchdog sources

The watchdog tracks named integer source IDs (`WatchdogSource`, a `typedef int64_t`). The built-in sources are:

| Constant | Value | Thread monitored |
|---|---|---|
| `WATCHDOG_SOURCE_MAIN_THREAD` | 0 | Application main loop |
| `WATCHDOG_SOURCE_RENDER_THREAD` | 1 | Flutter rasterizer thread |

Source IDs 3–255 are available for Dart-side registration via the platform channel.

#### Source name and timeout configuration

The `[watchdog]` table in `config.toml` is optional and accepts:

| Key | Type | Default | Description |
|---|---|---|---|
| `timeout_ms` | integer | `5000` | Watchdog timeout in milliseconds. Ignored when `BUILD_SYSTEMD_WATCHDOG=ON` (the `WatchdogSec=` unit interval takes precedence). |
| `source_names` | table | — | Maps source IDs (decimal string keys) to human-readable names used in log output. |

```toml
[watchdog]
timeout_ms = 10000

[watchdog.source_names]
1 = 'MyFlutterApp'
2 = 'BackgroundSync'
```

Source names can also be set or overridden at runtime by passing the optional `name` argument to the `start` platform channel method. The runtime value takes precedence over any config-defined name.


### Platform channel

When `BUILD_WATCHDOG=ON`, a `"watchdog"` platform channel is registered and available to Flutter apps via `StandardMethodCodec`. Methods:

| Method | Arguments | Description |
|---|---|---|
| `get_callbacks` | — | Returns a map of `start`, `pet`, `stop` native function pointers (FFI callable from Dart) |
| `start` | `{"source": int64, "name": string?}` | Register and begin monitoring source ID; optional `name` overrides any config-defined name for logging |
| `pet` | `{"source": int64}` | Reset the timeout for source ID |
| `stop` | `{"source": int64}` | Deregister source ID |

Source IDs passed from Dart must be non-negative. There is no upper-bound restriction.


### Example (Dart)

Use `get_callbacks` to retrieve native function pointers and call them directly from Dart FFI for zero-overhead petting on hot paths:

```dart
import 'dart:ffi';
import 'package:ffi/ffi.dart';

// Use method channels to get callbacks from the native watchdog implementation
NativeFunction<Void Function(Int64)>>? startCallback;
NativeFunction<Void Function(Int64)>>? petCallback;
NativeFunction<Void Function(Int64)>>? stopCallback;

void initWatchdog() async {
  final channel = MethodChannel('watchdog');
  final callbacks = await channel.invokeMethod<Map>('get_callbacks');

  final startCallbackPtr = Pointer.fromAddress(callbacks['start']);
  startCallback = startCallbackPtr.asFunction<void Function(int64)>();
  final petCallbackPtr = Pointer.fromAddress(callbacks['pet']);
  petCallback = petCallbackPtr.asFunction<void Function(int64)>();
  final stopCallbackPtr = Pointer.fromAddress(callbacks['stop']);
  stopCallback = stopCallbackPtr.asFunction<void Function(int64)>();
}

// example usage
const int WATCHDOG_SOURCE_APP = 123;

void main() {
  initWatchdog();
  // Start monitoring the main thread
  startCallback?.call(WATCHDOG_SOURCE_APP);
  // In your main loop, periodically pet the watchdog
  petCallback?.call(WATCHDOG_SOURCE_APP);
  // On shutdown, stop monitoring
  stopCallback?.call(WATCHDOG_SOURCE_APP);
}
```

## Crash Handler

Sentry-native support is available for Crash Handling. This pushes a mini-dump to the cloud for triage and tracking.

> To create user account and get a DSN
> see https://sentry.io/welcome/

### Configuration

1. Specify your DSN via environment variable `SENTRY_DSN`

2. Add the following sections to your `config.toml` file with the following structure:

```toml
[sentry]
release = "homescreen-1.0.0"
env = "production"
attachments = [
    "/path/to/crash.log",
    "/path/to/config.toml"
]

[sentry.tags]
platform = "linux"
device = "ivi"
```

### CMake Variables

To enable crash handler support:

    -DBUILD_CRASH_HANDLER=ON
    -DSENTRY_NATIVE_LIBDIR="directory where sentry native is installed, will look in CMAKE_INSTALL_PREFIX directory if not defined"
    -DCRASHPAD_BINARY_DIR="directory where crashpad_handler executable is installed, will look in CMAKE_INSTALL_PREFIX directory if not defined"

To resolve crash dump stack trace, debug binaries and symbols need to be uploaded to Sentry via sentry-cli tool: https://docs.sentry.io/cli/installation/

Required source repo:  https://github.com/getsentry/sentry-native

### Example Build steps

sentry build

```bash
git clone https://github.com/getsentry/sentry-native
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_STAGING_PREFIX=`pwd`/out/usr
make install
```

ivi-homescreen build

```bash
git clone https://github.com/toyota-connected/ivi-homescreen
mkdir build && cd build
cmake .. -DBUILD_CRASH_HANDLER=ON -DSENTRY_NATIVE_LIBDIR=`pwd`/../sentry-native/build/out/usr/lib -DCRASHPAD_BINARY_DIR=`pwd`/../sentry-native/build/out/usr/bin
make -j
SENTRY_DSN=<your DSN> homescreen --b=<your bundle folder> --f
```

## Yocto recipes

### Scarthgap

    https://github.com/meta-flutter/meta-flutter/tree/scarthgap/recipes-graphics/toyota

### Kirkstone

    https://github.com/meta-flutter/meta-flutter/tree/kirkstone/recipes-graphics/toyota

### Dunfell

    https://github.com/meta-flutter/meta-flutter/tree/dunfell/recipes-graphics/toyota
