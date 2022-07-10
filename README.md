# ivi-homescreen

[![Build Status](https://img.shields.io/github/workflow/status/toyota-connected/ivi-homescreen/ivi-homescreen-linux/main?logoColor=red&logo=ubuntu)](https://github.com/toyota-connected/ivi-homescreen/actions) [![Total alerts](https://img.shields.io/lgtm/alerts/g/toyota-connected/ivi-homescreen.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/toyota-connected/ivi-homescreen/alerts/) [![Language grade: C/C++](https://img.shields.io/lgtm/grade/cpp/g/toyota-connected/ivi-homescreen.svg?logo=lgtm&logoWidth=18)](https://lgtm.com/projects/g/toyota-connected/ivi-homescreen/context:cpp) [![Coverity Scan Build Status](https://scan.coverity.com/projects/23813/badge.svg)](https://scan.coverity.com/projects/toyota-connected-ivi-homescreen)


IVI Homescreen for Wayland

* Strongly Typed (C++)
* Lightweight
  * Clang 11 Release Stripped = 151k
  * GCC 9.3 Release Stripped = 168k
* Source runs on Desktop and Yocto Linux
  * Ubuntu 18+
  * Fedora 33+
  * Yocto Dunfell+
* Platform Channels enabled/disabled via CMake
* OpenGL Texture Framework

# Backend Support

### EGL Backend
This is the default build configuration.  To manually build EGL Backend use:
`-DBUILD_BACKEND_WAYLAND_EGL=ON -DBUILD_BACKEND_WAYLAND_VULKAN=OFF`

### Vulkan Backend
To build Vulkan Backend use:
`-DBUILD_BACKEND_WAYLAND_EGL=OFF -DBUILD_BACKEND_WAYLAND_VULKAN=ON`

# Bundle File Override Logic

If an override file is not present, it gets loaded from default location.

## Optional override files

### icudtl.dat

Bundle Override - `{bundle path}/data/icudtl.dat`

Yocto Default - `/usr/share/flutter/icudtl.dat`

Desktop Default - `/usr/local/share/flutter/icudtl.dat`

The CMake variable CMAKE_SYSROOT controls the sysroot prefix.
On Yocto this defaults to `/usr`, on desktop builds it defaults to `/usr/local`.

### libflutter_engine.so

Bundle Override - `{bundle path}/lib/libflutter_engine.so`

Yocto/Desktop Default - https://tldp.org/HOWTO/Program-Library-HOWTO/shared-libraries.html

# Command Line Options

`--a={int value}` - Sets the Engine's initial state of Accessibility Feature support.  Requires an integer value.

`--c` - Disables the cursor.

`--d` - Outputs backend debug information.  If Vulkan and Validation Layer is available, it will be loaded.

`--f` - Sets window to fullscreen.

`--w={int value}` - Sets View width.  Requires an integer value.

`--h={int value}` - Sets View height.  Requires an integer value.

`--t={String}` - Sets cursor theme to load.  e.g. --t=DMZ-White

`--b={path to folder}` - Sets the Bundle Path.  A bundle path expects the following folder structure:

```
  Flutter Application (bundle folder)
    data/flutter_assets
    data/icudtl.dat (optional - overrides system path)
    lib/libapp.so
    lib/libflutter_engine.so (optional - overrides system path)
```
* `--j=` - Sets the JSON configuration file.

* Dart VM arguments - any additional command line arguments not handled get directly passed to the Dart VM instance.

###JSON Configuration keys

#### Global

`app_id` - Sets Application ID.  Currently only the primary index app_id value is used.

`cursor_theme` - Sets cursor theme to use.

`disable_cursor` - Disables the cursor.

`debug_backend` - Enables Backend Debug logic.

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

Minimum definition when using `--j=`
```
{"view":{}}
```

If you used this minimum definition, invocation would look something like this
```
homescreen --j=/tmp/min_cfg.json --b={bundle path} --h={view height} --w={view width}
``` 

###JSON Configuration Example 1
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
###JSON Configuration Example 2
Loads Single View
1. Fullscreen Gallery app, cursor disabled, backend debug enabled, passing `vm_args` values to the Dart VM
```
/tmp/bg_dbg.json

{
   "disable_cursor":true,
   "backend_debug":true,
   "accessibility_features":31,
   "view":{
      "bundle_path":"/home/joel/development/gallery/.homescreen/x86/release",
      "vm_args":["--no-serve-devtools"],
      "width":1920,
      "height":1280,
      "fullscreen":true
   }
}

homescreen --j=/tmp/bg_dbg.json
```

### Parameter loading order
Only VM Command Line arguments are additive.  Meaning all instances of VM command line references will get added
together; JSON view + JSON global + CLI args.

All other parameters get assigned using the following ordering:

1. JSON Configuration View object parameters
2. JSON Configuration Global (non-view) parameters
3. Command Line parameters (Overrides View and Global parameters)

# x86_64 Desktop development notes

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

## GCC/libstdc++ Build

    git clone https://github.com/toyota-connected-na/ivi-homescreen.git
    mkdir build && cd build
    cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
    make install -j

## Clang/libc++ Build

    git clone https://github.com/toyota-connected-na/ivi-homescreen.git
    mkdir build && cd build
    CC=/usr/lib/llvm-12/bin/clang CXX=/usr/lib/llvm-12/clang++ cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
    make install -j

### Clang Toolchain Setup

    wget https://apt.llvm.org/llvm.sh
    chmod +x llvm.sh
    sudo ./llvm.sh 12
    sudo apt-get install -y libc++-12-dev libc++abi-12-dev libunwind-dev

## CI Example

    https://github.com/toyota-connected-na/ivi-homescreen/blob/main/.github/workflows/ivi-homescreen-linux.yml

## Debian Package

    make package -j
    sudo apt install ./ivi-homescreen-1.0.0-Release-beta-Linux-x86_64.deb

# Flutter Application
## Build

Confirm flutter/bin is in the path using: `flutter doctor -v`

    cd ~/development/my_flutter_app
    flutter channel beta
    flutter upgrade
    flutter config --enable-linux-desktop
    flutter create .
    flutter build bundle

## Install

loading path for application is: `/usr/local/share/homescreen/bundle`

This is used to leverage symlinks.  Such as:

    cd /usr/local/share/homescreen
    sudo rm -rf bundle
    sudo ln -sf ~/development/my_flutter_app/build/ bundle

Or

    sudo mkdir -p /usr/local/share/homescreen/my_flutter_app/
    sudo cp -r build/* /usr/local/share/homescreen/my_flutter_app/
    sudo ln -sf /usr/local/share/homescreen/my_flutter_app/ bundle

## Running on desktop

Copy a current icudtl.dat to /usr/local/share/flutter
Copy libflutter_engine.so to `/usr/local/lib` or use LD_LIBRARY_PATH to point downloaded engine for build:

    cd <homescreen build>
    export LD_LIBRARY_PATH=`pwd`:$LD_LIBRARY_PATH
    homescreen

## FPS
Three environmental variables impact FPS reporting.

### 1. FPS_OUTPUT_CONSOLE

Setting `FPS_OUTPUT_CONSOLE` to "1" will output FPS count to console.

### 2. FPS_OUTPUT_OVERLAY

If `FPS_OUTPUT_CONSOLE=1` and `FPS_OUTPUT_OVERLAY=1` the screen overlay is enabled.

### 3. FPS_OUTPUT_FREQUENCY

Changing `FPS_OUTPUT_FREQUENCY` controls the update interval.  This parameter is optional.

## Debug

Setup custom devices to control ivi-homescreen via debugger. 

# CMAKE dependency paths

Path prefix used to determine required files is determined at build.

For desktop `CMAKE_INSTALL_PREFIX` defaults to `/usr/local`
For target Yocto builds `CMAKE_INSTALL_PREFIX` defaults to `/usr`

# Yocto recipe

    https://github.com/jwinarske/meta-flutter/blob/dunfell/recipes-graphics/toyota/ivi-homescreen_git.bb
