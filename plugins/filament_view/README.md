# Filament View Plugin

This plugin is used with a AndroidView on the Flutter side

### Requirements
* Vulkan is supported by target system
* Build homescreen with Clag
  * This is due to the fact Clang is required for Filament.

_Filament does not build with GCC._

## Building Filament

Development work used the following configuration:

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
