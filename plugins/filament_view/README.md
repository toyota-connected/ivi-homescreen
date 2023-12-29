# Filament View

This plugin is built for use with an AndroidView on the Flutter side.

## Requirements

- **System Compatibility:** The target system must support Vulkan
- **Compiler:** Clang
    - Note: Filament requires Clang

## Building Filament

The development work is done using the following Filament configuration:

```
-DFILAMENT_SUPPORTS_WAYLAND=ON
-DFILAMENT_SUPPORTS_X11=OFF
-DFILAMENT_ENABLE_LTO=ON
-DFILAMENT_SUPPORTS_VULKAN=ON
-DFILAMENT_SUPPORTS_OPENGL=ON
-DFILAMENT_USE_EXTERNAL_GLES3=ON
-DFILAMENT_SUPPORTS_EGL_ON_LINUX=ON
-DFILAMENT_SKIP_SDL2=ON
-DFILAMENT_SKIP_SAMPLES=ON
-DFILAMENT_USE_SWIFTSHADER=OFF
-DBUILD_SHARED_LIBS=OFF
```

## Notes

Playx3d-scene needs conversion of assets for running on Vulkan+Linux:

```
cd filament
./cmake-build-debug-clang/tools/matc/matc --api vulkan \
-o <output file path>/flutter_assets/assets/materials/textured_pbr.filamat \
./samples/materials/groundShadow.mat
```