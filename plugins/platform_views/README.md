# Platform Views

This plugin supports the AndroidView interface. Meaning you can take any Platform View Dart code, change the definition
to use AndroidView, enable the plugin (`-DBUILD_PLUGIN_PLATFORM_VIEWS=ON`) and you will have a working interface.

Benefits of using this interface:

* pre-defined interface that is supported in Flutter SDK
* params are easily augmented in Dart to add for given use case
* pass touch to another process/library without additional work

For a backing implementation there are a number of approaches:

* Have a PlatformView implementation render to a Texture
* Compositor Region support (poke a hole for another process)
* Compositor sub-surface and manage Z-order

The above is already supported today using either an OpenGL texture, or a Compositor Sub-Surface. Using Platform Views
just re-uses an existing interface.
