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

* :gh:`Architecture document <docs/specs/ARCHITECTURE.md>`
* :gh:`Plugin ABI <docs/PLUGIN_ABI.md>`

Core subsystems
---------------

* :gh:`Configuration <shell/configuration/README.md>`
* :gh:`Views <shell/view/README.md>`
* :gh:`Displays <shell/display/README.md>`
* :gh:`Input <shell/input/README.md>`
* :gh:`Vsync <shell/vsync/README.md>`
* :gh:`Frame profiling <shell/profiling/README.md>`
* :gh:`Wayland integration <docs/specs/ARCHITECTURE.md#442-wayland-integration-and-compositor-protocol-shells>`
* :gh:`Logging and tracing <shell/logging/README.md>`

Rendering and presentation backends
-----------------------------------

* :gh:`Backend overview <shell/backend/README.md>`
* :gh:`Wayland EGL <shell/backend/wayland_egl/README.md>`
* :gh:`Wayland Vulkan <shell/backend/wayland_vulkan/README.md>`
* :gh:`DRM/KMS EGL <shell/backend/drm_kms_egl/README.md>`
* :gh:`DRM/KMS Vulkan <shell/backend/drm_kms_vulkan/README.md>`
* :gh:`Software backend <shell/backend/software/README.md>`
* :gh:`HUD overlay <shell/backend/hud/README.md>`

Platform integration
--------------------

* :gh:`Platform channels and embedder API <docs/specs/ARCHITECTURE.md#5-platform-channels--embedder-api>`
* :gh:`Platform views <shell/platform/homescreen/platform_views/README.md>`
* :gh:`Accessibility <docs/specs/ARCHITECTURE.md#63-accessibility>`
* :gh:`Shared plugin ABI library <shared/README.md>`

Optional features
-----------------

* :gh:`Watchdog <shell/watchdog/README.md>`
* :gh:`Crash handler <shell/crash_handler/README.md>`

Generated references
--------------------

.. toctree::
   :maxdepth: 2
   :caption: Generated references

   readme
   modules
