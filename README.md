# flutter-auto

[![Build Status](https://github.com/toyota-connected/ivi-homescreen/actions/workflows/flutter-auto-linux.yml/badge.svg)](https://github.com/toyota-connected/ivi-homescreen/actions/workflows/flutter-auto-linux.yml)


flutter-auto for Wayland

* Source runs on Desktop and Yocto Linux
  * Ubuntu 18+
  * Fedora 33+
  * Yocto Dunfell+
* Platform Channels enabled/disabled via CMake
* OpenGL Texture Framework
* Vulkan or GLES backend
* Runs on AGL Compositor, Weston, Sway, etc

# Sanitizer Support

You can enable the sanitizers with SANITIZE_ADDRESS, SANITIZE_MEMORY, SANITIZE_THREAD or SANITIZE_UNDEFINED options in your CMake configuration. You can do this by passing e.g. -DSANITIZE_ADDRESS=On on your command line.

If sanitizers are supported by your compiler, the specified targets will be build with sanitizer support. If your compiler has no sanitizing capabilities you'll get a warning but CMake will continue processing and sanitizing will simply just be ignored.

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

If an override file is not present, it gets loaded from default location.

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

`--a={int value}` - Sets the Engine's initial state of Accessibility Feature support.  Requires an integer value.

`--c` - Disables the cursor.

`--d` - Outputs backend debug information.  If Vulkan and Validation Layer is available, it will be loaded.

`--f` - Sets window to fullscreen.

`--w={int value}` - Sets View width.  Requires an integer value.

`--h={int value}` - Sets View height.  Requires an integer value.

`--p={int value}` - Sets Pixel Ratio.  Requires a double value.

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

### JSON Configuration keys

#### Global

`app_id` - Sets Application ID.  Currently only the primary index app_id value is used.

`cursor_theme` - Sets cursor theme to use.  This only applies to command line, and global parameter options.

`disable_cursor` - Disables the cursor.  This only applies to command line, and global parameter options.

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

`fps_output_console` - Setting to `1` FPS count is output to stdout.

`fps_output_overlay` - If `"fps_output_console"=1` and `"fps_output_overlay"=1` the screen overlay is enabled.

`fps_output_frequency` - Optional for FPS.  Changing value controls the update interval.

Minimum definition when using `--j=`
```
{"view":{}}
```

If you used this minimum definition, invocation would look something like this
```
flutter-auto --j=/tmp/min_cfg.json --b={bundle path} --h={view height} --w={view width}
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
         "bundle_path":"/home/joel/development/gallery/.flutter-auto/x86/release",
         "vm_args":["--enable-asserts", "--pause-isolates-on-start"],
         "window_type":"BG",
         "width":1920,
         "height":1280
      },
      {
         "bundle_path":"/home/joel/development/plugins/packages/video_player/video_player/example/.flutter-auto/x86/release",
         "window_type":"PANEL_LEFT",
         "width":320,
         "height":240,
         "accessibility_features":31
      }
   ]
}

flutter-auto --j=/tmp/bg_left_rel.json
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
      "bundle_path":"/home/joel/development/gallery/.flutter-auto/x86/release",
      "vm_args":["--no-serve-devtools"],
      "width":1920,
      "height":1280,
      "fullscreen":true
   }
}

flutter-auto --j=/tmp/bg_dbg.json
```

### Parameter loading order
Only VM Command Line arguments are additive.  Meaning all instances of VM command line references will get added
together; JSON view + JSON global + CLI args.

All other parameters get assigned using the following ordering:

1. JSON Configuration View object parameters
2. JSON Configuration Global (non-view) parameters
3. Command Line parameters (Overrides View and Global parameters)

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
    CC=/usr/lib/llvm-14/bin/clang CXX=/usr/lib/llvm-14/clang++ cmake .. -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
    make install -j

### Clang Toolchain Setup

    wget https://apt.llvm.org/llvm.sh
    chmod +x llvm.sh
    sudo ./llvm.sh 14
    sudo apt-get install -y libc++-14-dev libc++abi-14-dev libunwind-dev

## CI Example

    https://github.com/toyota-connected-na/ivi-homescreen/blob/main/.github/workflows/flutter-auto-linux.yml

## Debian Package

    make package -j
    sudo apt install ./flutter-auto-1.0.0-Release-beta-Linux-x86_64.deb

# Flutter Application
## Build

Confirm flutter/bin is in the path using: `flutter doctor -v`

    cd ~/development/my_flutter_app
    flutter channel stable
    flutter upgrade
    flutter config --enable-linux-desktop
    flutter create .
    flutter build bundle

## Install

create a bundle folder using this pattern:

    <bundle folder name>
        data
            flutter_assets - from flutter build folder
            [icudtl.dat] - optional
        lib
            [libflutter_engine.so] - optional


## Running on desktop

    flutter-auto --b=<bunle folder name>

## Debug

Setup custom devices to control flutter-auto via debugger. 

# CMAKE dependency paths

Path prefix used to determine required files is determined at build.

For desktop `CMAKE_INSTALL_PREFIX` defaults to `/usr/local`
For target Yocto builds `CMAKE_INSTALL_PREFIX` defaults to `/usr`


# Yocto recipes

## Kirkstone

    https://github.com/meta-flutter/meta-flutter/tree/kirkstone/recipes-graphics/toyota

## Dunfell

    https://github.com/meta-flutter/meta-flutter/tree/dunfell/recipes-graphics/toyota
