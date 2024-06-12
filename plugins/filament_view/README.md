# Filament View

This plugin is built for use with an AndroidView on the Flutter side.

## Requirements

- **System Compatibility:** The target system must support Vulkan
- **Compiler:** Clang
    - Note: Filament requires Clang

## Cloning Filament

git clone https://github.com/google/filament -b release
git checkout v1.52.3

## Building Filament

Apply patches found here:
https://github.com/meta-flutter/workspace-automation/tree/filament-dev/patches/filament

_Note: If building for desktop do not apply last patch_

Build using these flags:

```
-DFILAMENT_SUPPORTS_VULKAN=ON
-DFILAMENT_ENABLE_LTO=ON
-DFILAMENT_SUPPORTS_OPENGL=OFF
-DFILAMENT_USE_EXTERNAL_GLES3=OFF
-DFILAMENT_SUPPORTS_WAYLAND=ON
-DFILAMENT_SUPPORTS_X11=OFF
-DFILAMENT_SUPPORTS_XCB=OFF
-DFILAMENT_SUPPORTS_EGL_ON_LINUX=OFF
-DFILAMENT_SKIP_SDL2=ON
-DFILAMENT_SKIP_SAMPLES=ON
-DFILAMENT_USE_SWIFTSHADER=OFF
-DBUILD_SHARED_LIBS=OFF
-DCMAKE_STAGING_PREFIX=/mnt/raid10/filament/out/debug/usr
```

Make the install target to stage the required folders.

    ninja -C . install

Set ivi-homescreen variables to subfolders of the staged install:

    -DFILAMENT_INCLUDE_DIR=/mnt/raid10/filament/out/debug/include
    -DFILAMENT_LINK_LIBRARIES_DIR=/mnt/raid10/filament/out/debug/lib/x86_64

In above case the staged install is set to:

    /mnt/raid10/filament/out/debug/usr

## Notes

Playx3d-scene needs conversion of assets for running on Vulkan+Linux:

```
cd filament/cmake-build-debug-clang
./tools/matc/matc --api vulkan -o /home/joel/workspace-automation/app/playx-3d-scene/example/build/flutter_assets/assets/materials/textured_pbr.filamat ../samples/materials/groundShadow.mat
```