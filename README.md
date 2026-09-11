<div align="center">

<!-- template:in
# ${package_name}
${package_description}
 /template:in -->
<!-- template:out -->
# ivi-homescreen
Flutter embedder for Embedded Linux (C++)
<!-- /template:out -->

<!-- Badges -->

<!-- template:in
[![License: MPL 2.0](https://img.shields.io/badge/License-MPL_2.0-brightgreen.svg)](LICENSE)
[![build](https://github.com/${github_username}/${repo_name}/actions/workflows/package.yaml/badge.svg)](https://github.com/${github_username}/${repo_name}/actions/workflows/package.yaml)
[![example](https://github.com/${github_username}/${repo_name}/actions/workflows/example.yaml/badge.svg)](https://github.com/${github_username}/${repo_name}/actions/workflows/example.yaml)
[![stars](https://img.shields.io/github/stars/${github_username}/${repo_name}.svg)](https://github.com/${github_username}/${repo_name}/stargazers)
 /template:in -->
<!-- template:out -->
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-brightgreen.svg)](LICENSE)
[![build](https://github.com/toyota-connected/ivi-homescreen/actions/workflows/oss-ivi-homescreen.yml/badge.svg)](https://github.com/toyota-connected/ivi-homescreen/actions/workflows/oss-ivi-homescreen.yml)
[![docs](https://readthedocs.org/projects/ivi-homescreen/badge/?version=latest)](https://ivi-homescreen.readthedocs.io/en/latest/?badge=latest)
[![stars](https://img.shields.io/github/stars/toyota-connected/ivi-homescreen.svg)](https://github.com/toyota-connected/ivi-homescreen/stargazers)
[![Discord](https://img.shields.io/discord/1259897607531003945?style=plastic&logo=discord&label=Discord&color=%239656ce)
](https://discord.gg/V5uWD9fvws)
<!-- /template:out -->
</div>


### Use cases
- **Industrial deployments**: built for embedded environments requiring robustness and reliability (eg. automotive)
- **Consumer electronics**: embedded devices in consumer applications
- **Research and prototyping**: for experimental projects and proof-of-concept implementations


## Features
- Supports major desktop/embedded Linux platforms
    - **Yocto** master/Wrynose/Scarthgap/Kirkstone/Dunfell
    - **Ubuntu** 20.04, 22.04, 24.04
    - **Fedora** 42, 43, 44
    - **RaspberryPi OS** Bookworm, Trixie
- **Multiple backends**: any subset compiled in, one chosen at runtime <- [docs](shell/backend/README.md)
  - **Wayland**: EGL or Vulkan, with `xdg`, `agl`, `ivi` and RDK `simple` shell roles <- [docs](docs/specs/ARCHITECTURE.md#442-wayland-integration-and-compositor-protocol-shells)
  - **DRM/KMS**: EGL or Vulkan, direct scanout with no compositor <- [EGL](shell/backend/drm_kms_egl/README.md), [Vulkan](shell/backend/drm_kms_vulkan/README.md)
  - **Wayland leased DRM**: owns one connector leased from a running compositor (drm-lease-v1), e.g. a cluster panel beside an IVI compositor <- [docs](shell/backend/wayland_leased_drm/README.md)
  - **Headless**: EGL or Vulkan with no display, frames handed out as dma-buf to an encoder, WebRTC or an external consumer <- [EGL](shell/backend/headless_egl/README.md), [Vulkan](shell/backend/headless_vulkan/README.md)
  - **Software**: CPU rendering for CI and GPU-less boards; fbdev, DRM dumb buffer, PAM goldens or V4L2 encode <- [docs](shell/backend/software/README.md)
- **Multi-display**: multiple views across multiple outputs from one process; outputs matched by udev role name, EDID serial or connector, with hotplug <- [docs](shell/display/README.md)
- **Input**: libinput on DRM, Wayland seats otherwise; touch, keyboard, pointer routing across displays <- [docs](shell/input/README.md)
- **Configuration**: CLI flags layered over `config.toml` <- [docs](shell/configuration/README.md)
- **`PlatformView` framework and compositor** <- [docs](docs/specs/ARCHITECTURE.md#51-compositor-mode-and-platform-views)
  - Interleaves plugin-owned native surfaces with Flutter-rendered layers
  - Shared memory, zero-copy dma-buf import, or direct KMS overlay-plane scanout, with explicit sync <- [docs](docs/PLATFORM_VIEW_NEGOTIATION.md)
  - First party camera & video player plugins available at [`ivi-homescreen-plugins`](https://github.com/toyota-connected/ivi-homescreen-plugins/)
- **Accessibility** (optional): Flutter semantics for every running application, mirrored into an in-process tree that feeds the consumers below <- [docs](docs/specs/ARCHITECTURE.md#63-accessibility)
  - **AccessKit** (optional): exposes every application to screen readers over AT-SPI
  - **MCP** (optional): lets an agent (LLM, test harness) read and drive the UI over the Model Context Protocol <- [security](docs/mcp-security.md), [remote access](docs/mcp-remote-access.md)
    - Generic verbs over the semantics tree (`ui_query`, `ui_tap`, `ui_set_text`, `ui_scroll_to`, `ui_tap_at`); no app changes needed
    - Typed tools an app declares from Dart via [`ihs_mcp_app_tools`](packages/ihs_mcp_app_tools/README.md)
    - Off unless enabled at build time and at runtime; Unix socket only, peer-credential checked
    - [Cockpit demo](examples/cockpit_demo/README.md) for driving the shell from an LLM
- **OSGi multi-bundle framework** (optional): several Flutter bundles in one process with an OSGi lifecycle, priority-ordered startup and a shared service registry
- **Debug HUD**: Dear ImGui overlay with frame stats and each platform view's present path <- [docs](shell/backend/hud/README.md)
- **Frame profiling**: frame timing and motion-to-photon latency <- [docs](shell/profiling/README.md)
- **Watchdog** (optional): with optional SystemD support <- [docs](shell/watchdog/README.md)
- **Sentry-based crash handler** (optional) <- [docs](shell/crash_handler/README.md)
- **Logging/tracing**: with optional DLT support <- [docs](shell/logging/README.md), [DLT docs](shared/README.md#dlt-sink)
- **C Plugin ABI**: logging, tracing, platform views, semantics and MCP for out-of-tree plugins <- [docs](shared/README.md), [ABI contract](docs/PLUGIN_ABI.md)
- **Fuzzing**: coverage-guided fuzz targets for the surfaces that parse external input <- [docs](test/fuzz/README.md)

---

## 📚 Documentation

The documentation is organized around the system architecture and the
subsystems that implement it:

- [Read the documentation on Read the Docs](https://ivi-homescreen.readthedocs.io/en/latest/)
- [Architecture document](docs/specs/ARCHITECTURE.md)
- [Plugin ABI](docs/PLUGIN_ABI.md)
- [Subsystem documentation](docs/specs/ARCHITECTURE.md#4-features)

---

## 🔮 Usage Guide

### Setup & Build

Building via `emb_cli` is currently the recommended approach. `emb` will also automatically install the necessary dependencies for your host system.

See docs below for legacy instructions.

[Install `emb_cli`](https://github.com/toyota-connected/emb_cli#install) before proceeding with the build.

```bash
# Clone the repository
git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git

# Build the project
cd ivi-homescreen
emb cross . --target local --build
```

#### Build with plugins

To include first-party out-of-tree plugins available via [`ivi-homescreen-plugins`](https://github.com/toyota-connected/ivi-homescreen-plugins), clone the repository in a sibling folder and configure the necessary CMake variables:

```bash
cd ..
git clone https://github.com/toyota-connected/ivi-homescreen-plugins
cd ivi-homescreen

cmake -S . -B build \
-DDISABLE_PLUGINS=OFF \
-DPLUGINS_DIR=../ivi-homescreen-plugins
```

Alternatively, create an extended `emb` config that configures the plugin inclusion:

```yaml
cross:
  targets:
    rpi5-bookworm:
      extends: '../ivi-homescreen#rpi5-bookworm'   # ← project target (→ board)
      defines: { DISABLE_PLUGINS: 'OFF', PLUGINS_DIR: '../ivi-homescreen-plugins/plugins' }
      sysroot: { dev_packages: [ libnl-3-dev ] }
```

Then run the `emb cross` command again to build the project with the plugins included.

More info in `emb_cli` docs: https://github.com/toyota-connected/emb_cli#layered-manifests-extends-board--project--app


<details>
<summary>Legacy build instructions</summary>

### GCC/libstdc++ Build

Without plugins:

```bash
git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git
mkdir build && cd build
cmake ../ivi-homescreen -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
make install -j
```

With plugins:

```bash
git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git
git clone https://github.com/toyota-connected/ivi-homescreen-plugins.git
mkdir build && cd build
cmake ../ivi-homescreen -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local -DPLUGINS_DIR=`pwd`/ivi-homescreen-plugins
make install -j
```

### Clang/libc++ Build

Toolchain setup:

```bash
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 19
sudo apt-get install -y libc++-19-dev libc++abi-19-dev libunwind-dev
```

Without plugins:

```bash
git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git
mkdir build && cd build
CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake ../ivi-homescreen -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
make install -j
```

With plugins:

```bash
git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git
git clone https://github.com/toyota-connected/ivi-homescreen-plugins.git
mkdir build && cd build
CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake ../ivi-homescreen -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local -DPLUGINS_DIR=`pwd`/ivi-homescreen-plugins
make install -j
```
</details>

<details>
<summary>CMAKE dependency paths</summary>

Path prefix used to determine required files is determined at build.

For desktop `CMAKE_INSTALL_PREFIX` defaults to `/usr/local`
For target Yocto builds `CMAKE_INSTALL_PREFIX` defaults to `/usr`
</details>

<details>
<summary>CMake build flags</summary>

Below are some of the flags available for configuring the CMake build. Please note the most detailed & up-to-date doucumentation can be found via [`ARCHITECTURE.md`](docs/specs/ARCHITECTURE.md).

`ENABLE_XDG_CLIENT` - Enable XDG Client. Defaults to ON

`ENABLE_AGL_SHELL_CLIENT` - Enable AGL Client. Defaults to OFF

`ENABLE_IVI_SHELL_CLIENT` - Enable ivi-shell Client. Defaults to OFF

`ENABLE_SIMPLE_SHELL_CLIENT` - Enable RDK/Westeros simple_shell Client. Defaults to OFF

`BUILD_BACKEND_WAYLAND_LEASED_DRM` - Build the Wayland leased-DRM backend (drm-lease-v1). Must be paired with a renderer tier - see the [build matrix](shell/backend/README.md#build-matrix). Defaults to OFF

`ENABLE_LTO` - Enable Link Time Optimization. Defaults to OFF

`ENABLE_DLT` - Enable DLT logging. Defaults to OFF

`BUILD_BACKEND_WAYLAND_EGL` - Build Backend for EGL. Defaults to ON

`BUILD_EGL_TRANSPARENCY` - Build with EGL Transparency Enabled. Defaults to ON

`BUILD_EGL_ENABLE_3D` - Build with EGL Stencil, Depth, and Stencil config Enabled. Defaults to ON

`BUILD_EGL_ENABLE_MULTISAMPLE` - Build with EGL Sample set to 4. Defaults to OFF

`BUILD_BACKEND_WAYLAND_VULKAN` - Build Backend for Vulkan. Declared (default ON) only when `BUILD_BACKEND_WAYLAND_EGL=OFF`; can still be set explicitly alongside EGL

`BUILD_COMPOSITOR` - Enable the `FlutterCompositor` backing-store API so platform-view layers can be interleaved with Flutter UI. See [Compositor Mode and Platform Views](docs/specs/ARCHITECTURE.md#51-compositor-mode-and-platform-views). Defaults to OFF.

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

`BUILD_PLUGIN_DESKTOP_WINDOW_LINUX` - Includes Desktop Window Linux Plugin. Defaults to OFF

`BUILD_PLUGIN_FILE_SELECTOR` - Include File Selector plugin. Defaults to OFF

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
[Backend Support](shell/backend/README.md)).
</details>

<details>
<summary>Build as .deb package</summary>

```bash
make package -j
sudo apt install ./ivi-homescreen-1.0.0-Release-beta-Linux-x86_64.deb
```
</details>

### Run Flutter apps

<details>
<summary>Bundle structure & overrides</summary>

A bundle (`-b`) directory has this structure:

```
Flutter Application

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

Running the bundle above would be:

```bash
homescreen --b=`pwd`/.desktop-homescreen
```

If an override file is not present, it gets loaded from a default location.

##### `icudtl.dat`

Bundle Override

> `{bundle path}/data/icudtl.dat`

Yocto Default

> `/usr/share/flutter/icudtl.dat`

Desktop Default

> `/usr/local/share/flutter/icudtl.dat`

##### `libflutter_engine.so`

Bundle Override

> `{bundle path}/lib/libflutter_engine.so`

Yocto/Desktop Default - https://tldp.org/HOWTO/Program-Library-HOWTO/shared-libraries.html
</details>

Running via `emb_cli` is currently the recommended approach.
First, [install Flutter via `emb`](https://github.com/toyota-connected/emb_cli#emb-flutter).

Then, [create a bundle](https://github.com/toyota-connected/emb_cli#emb-bundle) and run it:

```bash
<path to ivi-homescreen>/build/shell/homescreen -b <path to bundle>
```

<details>
<summary>NVidia GL errors</summary>

Running EGL backend on a Lenovo Thinkpad with NVidia drivers may generate many GL runtime errors.
This should resolve it:

```bash
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
```
</details>

#### CLI opts and configuration

All CLI flags, the full `config.toml` reference, the schema walkthrough, the parameter loading order, and multi-display examples now live with the configuration subsystem's documentation:
[`shell/configuration/README.md`](shell/configuration/README.md).



---

## 📄 License

This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.

## Contributors 🧑‍💻💙📝

This package is developed/maintained by the following people

<!-- template:in
![contributors badge](https://readme-contribs.as93.net/contributors/${github_username}/${repo_name}?textColor=888888)
 /template:in -->
<!-- template:out -->
![contributors badge](https://readme-contribs.as93.net/contributors/toyota-connected/ivi-homescreen?textColor=888888)
<!-- /template:out -->
