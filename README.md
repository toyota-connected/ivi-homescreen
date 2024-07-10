# ivi-homescreen

Flutter Linux CPP Embedder

### Discord Server https://discord.gg/V5uWD9fvws

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
* Backend Support
  * EGL
  * Vulkan (first Flutter embedder to support this)
  * Wayland Leased DRM (coming soon)
  * DRM/KMS (coming soon)
* Same source code runs on Desktop and embedded Linux image
  * Ubuntu 18+
  * Fedora 33+
  * Yocto Dunfell/Kirkstone/Scarthgap


## Plugins
ivi-homescreen plugins are located at https://github.com/toyota-connected/plugins

There are two ways to reference this repo:
1. Clone plugins repo to root of ivi-homescreen folder
2. Set PLUGIN_DIR to repo path.  -DPLUGIN_DIR=<my path>

## Logging

Logging level support
* trace
* debug
* info
* warn
* error
* critical
* off

If environmental variable SPDLOG_LEVEL is not set, logging defaults to info.

To set logging to trace use

    SPDLOG_LEVEL=trace

To set logging to debug use

    SPDLOG_LEVEL=debug

# DLT logging

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

# Sanitizer Support

You can enable the sanitizers with SANITIZE_ADDRESS, SANITIZE_MEMORY, SANITIZE_THREAD or SANITIZE_UNDEFINED options in your CMake configuration. You can do this by passing e.g. -DSANITIZE_ADDRESS=On on your command line.

If sanitizers are supported by your compiler, the specified targets will be built with sanitizer support. If your compiler has no sanitizing capabilities you'll get a warning but CMake will continue processing and sanitizing will simply just be ignored.

# Backend Support

### EGL Backend
This is the default build configuration.  To manually build EGL Backend use
```
-DBUILD_BACKEND_WAYLAND_EGL=ON -DBUILD_BACKEND_WAYLAND_VULKAN=OFF
```

### Vulkan Backend
To build Vulkan Backend use
```
-DBUILD_BACKEND_WAYLAND_EGL=OFF -DBUILD_BACKEND_WAYLAND_VULKAN=ON
```

Running Vulkan requires an engine version that supports Vulkan.  Stable does not yet support Vulkan.

# Bundle File Override Logic

If an override file is not present, it gets loaded from a default location.

## Optional override files

### icudtl.dat

Bundle Override

    {bundle path}/data/icudtl.dat

Yocto Default

    /usr/share/flutter/icudtl.dat

Desktop Default

    /usr/local/share/flutter/icudtl.dat

### libflutter_engine.so

Bundle Override

    {bundle path}/lib/libflutter_engine.so

Yocto/Desktop Default - https://tldp.org/HOWTO/Program-Library-HOWTO/shared-libraries.html

# Command Line Options

`-a {int value}` - Sets the Engine's initial state of Accessibility Feature support.  Requires an integer value.

`-disable-cursor` - Disables the cursor.

`-d` - Outputs backend debug information.  If Vulkan and Validation Layer are available, it will be loaded.

`-f` - Sets the window to fullscreen.

`-w {int value}` - Sets View width.  Requires an integer value.

`-h {int value}` - Sets View height.  Requires an integer value.

`-i {int value}` - Sets ivi-shell surface ID.  Requires an integer value.

`-p {int value}` - Sets Pixel Ratio.  Requires a double value.

`-t {String}` - Sets cursor theme to load.  e.g. -t DMZ-White

`-b {path to folder}` - Sets the Bundle Path.  A bundle path expects the following folder structure:
```
  Flutter Application (bundle folder)
    data/flutter_assets
    data/icudtl.dat (optional - overrides system path)
    lib/libapp.so
    lib/libflutter_engine.so (optional - overrides system path)
```
* `-j` - Sets the JSON configuration file.
* `-wayland-event-mask` - Sets events to ignore. e.g. -wayland-event-mask pointer-axis, or --wayland-event-mask="pointer-axis, touch"

  * Available parameters are:
      pointer, pointer-axis, pointer-buttons, pointer-motion, keyboard, touch

* Dart VM arguments - any additional command line arguments not handled get directly passed to the Dart VM instance.

### JSON Configuration keys

#### Global

`app_id` - Sets Application ID.  Currently only the primary index app_id value is used.

`cursor_theme` - Sets cursor theme to use.  This only applies to command line, and global parameter options.

`disable_cursor` - Disables the cursor.  This only applies to command line, and global parameter options.

`debug_backend` - Enables Backend Debug logic.

`wayland_event_mask` - See command line option --wayland-event-mask

#### View Specific

`view` - Minimum required.  Can be single object or array.

`bundle_path` - sets the Bundle Path.

`window_type` - Currently used for AGL Compositor Window Types.  If not running on AGL compositor,
it will create borderless windows in no particular position.

`width` - sets View width.  Requires an integer value.

`height` - sets View height.  Requires an integer value.

`accessibility_features` - Bitmask of Engine Accessibility Features.  Requires an integer.  See flutter_embedder.h for valid values.

`vm_args` - Array of strings which get passed to the VM instance as command line arguments.

`fullscreen` - Sets window to fullscreen.

`ivi_surface_id` - Sets ivi-shell surface ID.

`fps_output_console` - Setting to `1` FPS count is output to stdout.

`fps_output_overlay` - If `"fps_output_console"=1` and `"fps_output_overlay"=1` the screen overlay is enabled.

`fps_output_frequency` - Optional for FPS.  Changing value controls the update interval.

Minimum definition when using `-j`
```
{"view":{}}
```

If you used this minimum definition, invocation would look something like this
```
homescreen -j /tmp/min_cfg.json -b {bundle path} -h {view height} -w {view width}
``` 

### JSON Configuration Example 1

Loads Two Views
1. Gallery app to a 1920x1280 Background window, passing two arguments to the Dart VM
2. Video player to Left Panel sized 320x240 with all accessibility features enabled.
```
/tmp/bg_left_rel.json

{
   "view":[
      {
         "bundle_path":"/home/joel/development/gallery/.homescreen/x86/release",
         "vm_args":["--enable-asserts", "--pause-isolates-on-start"],
         "window_type":"BG",
         "width":1920,
         "height":1280
      },
      {
         "bundle_path":"/home/joel/development/plugins/packages/video_player/video_player/example/.homescreen/x86/release",
         "window_type":"PANEL_LEFT",
         "width":320,
         "height":240,
         "accessibility_features":31
      }
   ]
}

homescreen --j=/tmp/bg_left_rel.json
```

### JSON Configuration Example 2

Loads Single View
1. Fullscreen Gallery app, cursor disabled, backend debug enabled, passing `vm_args` values to the Dart VM
```
/tmp/bg_dbg.json

{
   "disable_cursor":true,
   "debug_backend":true,
   "accessibility_features":31,
   "view":{
      "bundle_path":"/home/joel/development/gallery/.homescreen/x86/release",
      "vm_args":["--no-serve-devtools"],
      "width":1920,
      "height":1280,
      "fullscreen":true
   }
}

homescreen -j /tmp/bg_dbg.json
```

### Parameter loading order
Only VM Command Line arguments are additive.  Meaning all instances of VM command line references will get added
together; JSON view + JSON global + CLI args.

All other parameters get assigned using the following ordering:

1. JSON Configuration View object parameters
2. JSON Configuration Global (non-view) parameters
3. Command Line parameters (Overrides View and Global parameters)

# CMake Build flags
| **Flag**                                | **Default Value** | **Description**                                                                 |
|-----------------------------------------|-------------------|---------------------------------------------------------------------------------|
| `ENABLE_XDG_CLIENT`                     | ON                | Enable XDG Client.                                                             |
| `ENABLE_AGL_SHELL_CLIENT`               | OFF               | Enable AGL Client.                                                             |
| `ENABLE_IVI_SHELL_CLIENT`               | OFF               | Enable ivi-shell Client.                                                       |
| `ENABLE_DRM_LEASE_CLIENT`               | OFF               | Enable drm lease Client.                                                       |
| `ENABLE_LTO`                            | OFF               | Enable Link Time Optimization.                                                 |
| `ENABLE_DLT`                            | OFF               | Enable DLT logging.                                                            |
| `BUILD_BACKEND_WAYLAND_EGL`             | ON                | Build Backend for EGL.                                                         |
| `BUILD_EGL_TRANSPARENCY`                | ON                | Build with EGL Transparency Enabled.                                           |
| `BUILD_EGL_ENABLE_3D`                   | ON                | Build with EGL Stencil, Depth, and Stencil config Enabled.                     |
| `BUILD_EGL_ENABLE_MULTISAMPLE`          | ON                | Build with EGL Sample set to 4.                                                |
| `BUILD_BACKEND_WAYLAND_VULKAN`          | OFF               | Build Backed for Vulkan.                                                       |
| `BUILD_BACKEND_HEADLESS_EGL`            | OFF               | Build Headless backend for EGL (OSMesa).                                       |
| `DEBUG_PLATFORM_MESSAGES`               | OFF               | Dump Platform Channel Messages.                                                |
| `BUILD_CRASH_HANDLER`                   | OFF               | Build Sentry IO Crash Handler Support.                                         |
| `BUILD_DOCS`                            | OFF               | Builds Docs.                                                                   |
| `BUILD_UNIT_TESTS`                      | OFF               | Build Unit Tests.                                                              |
| `UNIT_TEST_SAVE_GOLDENS`                | OFF               | Update test goldens.                                                           |
| `EXE_OUTPUT_NAME`                       | `homescreen`      | Set executable output name.                                                    |
| `DISABLE_PLUGINS`                       | OFF               | Disables all plugins located in the plugins folder.                            |
| `BUILD_PLUGIN_AUDIOPLAYERS_LINUX`       | OFF               | Include Audioplayers Linux plugin.                                             |
| `BUILD_PLUGIN_CAMERA`                   | OFF               | Include Camera plugin.                                                         |
| `BUILD_PLUGIN_CLOUD_FIRESTORE`          | OFF               | Plugin Cloud Firestore.                                                        |
| `BUILD_PLUGIN_DESKTOP_WINDOW_LINUX`     | OFF               | Includes Desktop Window Linux Plugin.                                          |
| `BUILD_PLUGIN_FILE_SELECTOR`            | OFF               | Include File Selector plugin.                                                  |
| `BUILD_PLUGIN_FIREBASE_AUTH`            | OFF               | Plugin Firebase Auth.                                                          |
| `BUILD_PLUGIN_FIREBASE_STORAGE`         | OFF               | Plugin Firebase Storage.                                                       |
| `BUILD_PLUGIN_GO_ROUTER`                | ON                | Includes Go Router Plugin.                                                     |
| `BUILD_PLUGIN_GOOGLE_SIGN_IN`           | OFF               | Include Google Sign In manager.                                                |
| `BUILD_PLUGIN_INTEGRATION_TEST`         | OFF               | Included Flutter Integration Test support.                                     |
| `BUILD_PLUGIN_PDF`                      | OFF               | Include PDF plugin.                                                            |
| `BUILD_PLUGIN_SECURE_STORAGE`           | OFF               | Includes Flutter Secure Storage.                                               |
| `BUILD_PLUGIN_URL_LAUNCHER`             | OFF               | Includes URL Launcher Plugin.                                                  |
| `BUILD_PLUGIN_VIDEO_PLAYER_LINUX`       | OFF               | Include Video Player plugin.                                                   |
| `BUILD_PLUGIN_FILAMENT_VIEW`            | OFF               | Include Filament View plugin.                                                  |
| `BUILD_PLUGIN_LAYER_PLAYGROUND_VIEW`    | OFF               | Include Layer Playground View plugin.                                          |
| `BUILD_PLUGIN_NAV_RENDER_VIEW`          | OFF               | Include Navigation Render View plugin.                                         |
| `BUILD_PLUGIN_WEBIVEW_FLUTTER_VIEW`     | OFF               | Includes WebView View Plugin.                                                  |

_**Backend selections (Vulkan, EGL/GLESv2) are mutually exclusive by design.**_

# x86_64 Desktop development notes

## NVidia GL errors

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

# Build steps

## Required Packages

    sudo add-apt-repository ppa:kisak/kisak-mesa
    sudo apt-get update -y
    sudo apt-get -y install libwayland-dev wayland-protocols \
    mesa-common-dev libegl1-mesa-dev libgles2-mesa-dev mesa-utils \
    libxkbcommon-dev

## Optional Packages

    # To build doxygen documentation
    sudo apt-get -y install doxygen

## GCC/libstdc++ Build

    git clone https://github.com/toyota-connected-na/ivi-homescreen.git
    mkdir build && cd build
    cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
    make install -j

## Clang/libc++ Build

    git clone https://github.com/toyota-connected-na/ivi-homescreen.git
    mkdir build && cd build
    CC=/usr/lib/llvm-12/bin/clang CXX=/usr/lib/llvm-12/bin/clang++ cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
    make install -j

### Clang Toolchain Setup

    wget https://apt.llvm.org/llvm.sh
    chmod +x llvm.sh
    sudo ./llvm.sh 14
    sudo apt-get install -y libc++-14-dev libc++abi-14-dev libunwind-dev

## CI Example

    https://github.com/toyota-connected-na/ivi-homescreen/blob/main/.github/workflows/ivi-homescreen-linux.yml

## Debian Package

    make package -j
    sudo apt install ./ivi-homescreen-1.0.0-Release-beta-Linux-x86_64.deb

# Flutter Application

## Running an app

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


# CMAKE dependency paths

Path prefix used to determine required files is determined at build.

For desktop `CMAKE_INSTALL_PREFIX` defaults to `/usr/local`
For target Yocto builds `CMAKE_INSTALL_PREFIX` defaults to `/usr`

# Crash Handler

Sentry-native support is available for Crash Handling.  This pushes a mini-dump to the cloud for triage and tracking.

To create user account and get DNS See https://sentry.io/welcome/

Required CMake Variables

    -DBUILD_CRASH_HANDLER=ON
    -DCRASH_HANDLER_DSN="dsn from your account"

Required source repo:  https://github.com/getsentry/sentry-native

### Example Build steps

sentry build

    git clone https://github.com/getsentry/sentry-native
    mkdir build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_STAGING_PREFIX=`pwd`/out/usr
    make install

ivi-homescreen build

    git clone https://github.com/toyota-connected/ivi-homescreen
    mkdir build && cd build
    cmake .. -DBUILD_CRASH_HANDLER=ON -DCRASH_HANDLER_DSN="dsn from your account"
    make -j
    LD_LIBRARY_PATH=<sentry staged sysroot install path>/lib homescreen --b=<your bundle folder> --f

# Yocto recipes

## Scarthgap

    https://github.com/meta-flutter/meta-flutter/tree/scarthgap/recipes-graphics/toyota

## Kirkstone

    https://github.com/meta-flutter/meta-flutter/tree/kirkstone/recipes-graphics/toyota

## Dunfell

    https://github.com/meta-flutter/meta-flutter/tree/dunfell/recipes-graphics/toyota
