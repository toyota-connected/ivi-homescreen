.. ivi-homescreen documentation master file

ivi-homescreen
==========================================

This documentation is organized by purpose:

* The architecture document explains how the application is structured.
* Subsystem guides explain the implementation and configuration of individual
  components.
* Generated API/module documentation is available in the module reference.

Architecture
------------

The main architectural overview is maintained with the source documentation:

* `Architecture document <https://github.com/toyota-connected/ivi-homescreen/blob/main/docs/specs/ARCHITECTURE.md>`_
* `Plugin ABI <https://github.com/toyota-connected/ivi-homescreen/blob/main/docs/PLUGIN_ABI.md>`_

Core subsystems
---------------

* `Configuration <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/configuration/README.md>`_
* `Views <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/view/README.md>`_
* `Displays <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/display/README.md>`_
* `Input <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/input/README.md>`_
* `Vsync <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/vsync/README.md>`_
* `Frame profiling <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/profiling/README.md>`_
* `Wayland integration <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/wayland/README.md>`_
* `Logging and tracing <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/logging/README.md>`_

Rendering and presentation backends
-----------------------------------

* `Backend overview <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/backend/README.md>`_
* `Wayland EGL <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/backend/wayland_egl/README.md>`_
* `Wayland Vulkan <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/backend/wayland_vulkan/README.md>`_
* `DRM/KMS EGL <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/backend/drm_kms_egl/README.md>`_
* `DRM/KMS Vulkan <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/backend/drm_kms_vulkan/README.md>`_
* `Software backend <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/backend/software/README.md>`_
* `HUD overlay <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/backend/hud/README.md>`_

Platform integration
--------------------

* `Platform channels and embedder API <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/platform/homescreen/README.md>`_
* `Platform views <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/platform/homescreen/platform_views/README.md>`_
* `Accessibility <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/accessibility/README.md>`_
* `Shared plugin ABI library <https://github.com/toyota-connected/ivi-homescreen/blob/main/shared/README.md>`_

Optional features
-----------------

* `Watchdog <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/watchdog/README.md>`_
* `Crash handler <https://github.com/toyota-connected/ivi-homescreen/blob/main/shell/crash_handler/README.md>`_

Generated references
--------------------

.. toctree::
   :maxdepth: 2
   :caption: Generated references

   readme
   modules