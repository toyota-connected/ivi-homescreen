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
<!-- /template:out -->

**Discord Server: https://discord.gg/V5uWD9fvws**

</div>

### Use cases
- **Industrial deployments**: built for embedded environments requiring robustness and reliability (eg. automotive)
- **Consumer electronics**: embedded devices in consumer applications
- **Research and prototyping**: for experimental projects and proof-of-concept implementations


## Features
- Supports major desktop/embedded Linux platforms
    - **Yocto** Dunfell/Kirkstone/Scarthgap
    - **Ubuntu** 20.04, 22.04, 24.04
    - **Fedora** 42, 43, 44
    - **RaspberryPi OS** Bookworm, Trixie
- **Multiple backend support**: Wayland/DRM-KMS, Vulkan/EGL/Software <- [docs](shell/backend/README.md)
  - **Wayland integration & Compositor-Protocol shells** <- [docs](docs/specs/ARCHITECTURE.md#442-wayland-integration-and-compositor-protocol-shells)
- **Multi-display**: Multiple views across multiple outputs from one process <- [docs](shell/display/README.md)
- **`PlatformView` framework and compositor** <- [docs](docs/specs/ARCHITECTURE.md#51-compositor-mode-and-platform-views)
  - Supports interleaving plugin-owned native surfaces with Flutter-rendered layers
  - First party camera & video player plugins available at [`ivi-homescreen-plugins`](https://github.com/toyota-connected/ivi-homescreen-plugins/)
- **Watchdog** (optional): with optional SystemD support <- [docs](shell/watchdog/README.md)
- **Sentry-based crash handler** (optional) <- [docs](shell/crash_handler/README.md)
- **Accessibility support** <- [docs](docs/specs/ARCHITECTURE.md#63-accessibility)
- **Logging/tracing**: with optional DLT support <- [docs](shell/logging/README.md), [DLT docs](shared/README.md#dlt-sink)
- **C Plugin ABI** <- [docs](shared/README.md)

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
