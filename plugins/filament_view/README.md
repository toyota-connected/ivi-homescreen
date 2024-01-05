# Filament View

This plugin is built for use with an AndroidView on the Flutter side.

## Requirements

- **System Compatibility:** The target system must support Vulkan
- **Compiler:** Clang
    - Note: Filament requires Clang

## Building Filament

Apply the three patches located here:
https://github.com/jwinarske/meta-vulkan/tree/kirkstone/recipes-graphics/filament/files

Build using these flags:

```
-DFILAMENT_SUPPORTS_VULKAN=ON
-DFILAMENT_ENABLE_LTO=ON
-DFILAMENT_SUPPORTS_OPENGL=OFF
-DFILAMENT_USE_EXTERNAL_GLES3=OFF
-DFILAMENT_SUPPORTS_WAYLAND=ON
-DFILAMENT_SUPPORTS_X11=OFF
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

    -DFILAMENT_INCLUDE_DIR=/mnt/raid10/filament/out/debug/usr/include
    -DFILAMENT_LINK_LIBRARIES_DIR=/mnt/raid10/filament/out/debug/usr/lib/x86_64

In above case the staged install is set to:

    /mnt/raid10/filament/out/debug/usr

## Notes

Playx3d-scene needs conversion of assets for running on Vulkan+Linux:

```
cd filament
./cmake-build-debug-clang/tools/matc/matc --api vulkan \
-o <output file path>/flutter_assets/assets/materials/textured_pbr.filamat \
./samples/materials/groundShadow.mat
```