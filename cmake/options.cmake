#
# Copyright 2020 Toyota Connected North America
# @copyright Copyright (c) 2022 Woven Alpha, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

include_guard()

# repo tags

if (NOT CMAKE_APPS_MODULE_TAG)
    set(CMAKE_APPS_MODULE_TAG master)
endif ()

# determine if CI or local build

if (DEFINED ENV{CI})
    MESSAGE(STATUS "Build Type ............. CI - $ENV{CI_JOB_NAME}")
    set(BUILD_TYPE_LOCAL OFF)
    set(BUILD_TYPE_CI ON)
else ()
    MESSAGE(STATUS "Build Type ............. LOCAL")
    set(BUILD_TYPE_CI OFF)
    set(BUILD_TYPE_LOCAL ON)
endif ()

#
# variables
#
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third_party)

#
# LTO
#
option(ENABLE_LTO "Enable Link Time optimization" OFF)

#
# DLT
#
option(ENABLE_DLT "Enable DLT logging" OFF)

#
# Plugin Override
#
option(DISABLE_PLUGINS "Disable Plugins" OFF)

if (NOT PLUGINS_DIR)
    set(PLUGINS_DIR ${CMAKE_SOURCE_DIR}/ivi-homescreen-plugins)
endif ()

if (NOT DISABLE_PLUGINS AND EXISTS ${PLUGINS_DIR})
    MESSAGE(STATUS "Plugins ................ Enabled")
    set(ENABLE_PLUGINS ON)
elseif (DISABLE_PLUGINS OR NOT EXISTS ${PLUGINS_DIR})
    MESSAGE(STATUS "Plugins ................ Disabled")
    set(ENABLE_PLUGINS OFF)
endif ()

#
# backend selection
#
option(BUILD_BACKEND_WAYLAND_EGL "Build Backend for EGL" ON)
if (BUILD_BACKEND_WAYLAND_EGL)
    option(BUILD_EGL_TRANSPARENCY "Build with EGL Transparency Enabled" ON)
    option(BUILD_EGL_ENABLE_3D "Build with EGL Stencil, Depth, and Stencil config Enabled" ON)
    option(BUILD_EGL_ENABLE_MULTISAMPLE "Build with EGL Sample set to 4" OFF)
else ()
    option(BUILD_BACKEND_WAYLAND_VULKAN "Build Backend for Vulkan" ON)
endif ()

#
# FlutterCompositor backing-store API. When OFF (default), the backend uses
# single-surface rendering. When ON, platform-view layer interleaving is
# enabled via the compositor callbacks.
#
option(BUILD_COMPOSITOR "Enable FlutterCompositor backing store API" OFF)

#
# Export Vulkan backing-store memory as a DMA-BUF fd so plugins can import
# it zero-copy. Requires VK_KHR_external_memory_fd at runtime; silently
# falls back to a non-exportable allocation if unavailable.
#
option(BUILD_COMPOSITOR_DMABUF_EXPORT
        "Export Vulkan backing stores as DMA-BUF for zero-copy plugins" OFF)

#
# DRM
#
option(BUILD_BACKEND_WAYLAND_LEASED_DRM "Build Wayland Leased DRM backend" OFF)
option(BUILD_BACKEND_DRM_KMS_EGL
        "Build DRM/KMS EGL backend (mutually exclusive with EGL and Vulkan backends)"
        OFF)
option(BUILD_BACKEND_DRM_KMS_VULKAN
        "Build DRM/KMS Vulkan backend (mutually exclusive with the EGL and Wayland backends)"
        OFF)

# Vendor VK_LAYER_KHRONOS_validation into the Vulkan backend so -d guarantees
# validation even on images that ship no system layer registry. Off by default;
# flip ON for dev/CI presets. The option is declared here so the build graph is
# stable ahead of the in-process layer wiring.
option(BUILD_VULKAN_VALIDATION
        "Vendor the Khronos Vulkan validation layer into the Vulkan backend"
        OFF)

# Standalone zero-copy-capability probe (drm_kms_vulkan_probe). Builds just the
# probe tool without selecting the DRM/KMS Vulkan backend, so it can be
# cross-built and run on a target — alongside any backend — to report whether
# the device can do zero-copy dma-buf scanout. Implied ON when the DRM/KMS
# Vulkan backend itself is built.
option(BUILD_DRM_KMS_VULKAN_PROBE
        "Build the standalone drm_kms_vulkan zero-copy capability probe"
        OFF)

# Drive the non-framed DRM/KMS present path through drm::scene::LayerScene
# rather than the in-tree PlaneRegistry + Allocator + AtomicRequest pipeline.
# Off-by-default while the integration is bedding in; flip to ON to exercise
# LayerScene-managed plane allocation, composition fallback, and session
# pause/resume forwarding. The framed-mode path is unaffected by this flag.
option(USE_DRM_SCENE
        "Drive DRM/KMS non-framed present path via drm::scene::LayerScene"
        OFF)
if (USE_DRM_SCENE AND NOT (BUILD_BACKEND_DRM_KMS_EGL OR BUILD_BACKEND_DRM_KMS_VULKAN))
    message(FATAL_ERROR
            "USE_DRM_SCENE=ON requires BUILD_BACKEND_DRM_KMS_EGL=ON or "
            "BUILD_BACKEND_DRM_KMS_VULKAN=ON")
endif ()

#
# Headless (software / CPU renderer — no GPU or Wayland)
#
option(BUILD_BACKEND_HEADLESS_SOFTWARE
        "Build headless backend using kSoftware renderer — no GPU or Wayland required"
        OFF)
if (BUILD_BACKEND_HEADLESS_SOFTWARE)
    # Force all hardware sinks and libinput off: headless has no display
    # device to scan out to and no /dev/input/event* to listen on.
    set(BUILD_SOFTWARE_SINK_DRM OFF CACHE BOOL "" FORCE)
    set(BUILD_SOFTWARE_SINK_FBDEV OFF CACHE BOOL "" FORCE)
    set(BUILD_SOFTWARE_INPUT_LIBINPUT OFF CACHE BOOL "" FORCE)
endif ()

#
# Software (CPU rendering; no GPU, no display server)
#
option(BUILD_BACKEND_SOFTWARE
        "Build software (CPU) backend — kSoftware renderer, no GPU/display required"
        OFF)
if (BUILD_BACKEND_SOFTWARE)
    # Optional DRM dumb-buffer sink. Defaults ON when libdrm is
    # available (typical Linux dev/CI host) so the sink is reachable
    # without an explicit cmake flag. Auto-disables on systems
    # without libdrm; can also be force-OFF for a minimal CI image.
    find_package(PkgConfig)
    pkg_check_modules(DRM_DUMB libdrm IMPORTED_TARGET)
    option(BUILD_SOFTWARE_SINK_DRM
            "Build the DRM dumb-buffer sink for the software backend"
            ${DRM_DUMB_FOUND})
    if (BUILD_SOFTWARE_SINK_DRM AND NOT DRM_DUMB_FOUND)
        message(FATAL_ERROR
                "BUILD_SOFTWARE_SINK_DRM=ON but pkg-config libdrm was not found")
    endif ()
    # Optional /dev/fb* sink. Linux-only, no library dependency beyond
    # the kernel uapi headers (linux/fb.h) which ship with every libc.
    # Default ON when targeting Linux; the build hosts targeting other
    # OSes (the embedder targets Linux today, but defensive) can flip
    # it off via -DBUILD_SOFTWARE_SINK_FBDEV=OFF.
    option(BUILD_SOFTWARE_SINK_FBDEV
            "Build the fbdev (/dev/fb*) sink for the software backend"
            ON)
    # Optional libinput-backed seat for keyboard / pointer events.
    # Auto-on if pkg-config finds libinput + libudev + xkbcommon (the
    # universal Linux desktop input stack). Force-on without the deps
    # is a fatal configure error.
    pkg_check_modules(SW_LIBINPUT libinput libudev xkbcommon IMPORTED_TARGET)
    option(BUILD_SOFTWARE_INPUT_LIBINPUT
            "Build the libinput-backed input seat for the software backend"
            ${SW_LIBINPUT_FOUND})
    if (BUILD_SOFTWARE_INPUT_LIBINPUT AND NOT SW_LIBINPUT_FOUND)
        message(FATAL_ERROR
                "BUILD_SOFTWARE_INPUT_LIBINPUT=ON but pkg-config could not "
                "find libinput / libudev / xkbcommon")
    endif ()
endif ()

option(DEBUG_PLATFORM_MESSAGES "Debug platform messages" OFF)

#
# Crash Handler
#
option(BUILD_CRASH_HANDLER "Build Crash Handler" OFF)
if (BUILD_CRASH_HANDLER)
    message(STATUS "Crash Handler .......... Enabled")

    include(GNUInstallDirs)

    # sentry-native installs its CMake package config under the GNUInstallDirs
    # libdir of its staging prefix -- typically lib/cmake/sentry, but lib64 on
    # some distros and a flat cmake/sentry on older layouts. Hardcoding a single
    # suffix breaks whenever ${CMAKE_INSTALL_LIBDIR} here differs from the one
    # sentry-native installed with (or is empty because GNUInstallDirs had not
    # been included yet). Search the likely suffixes under the chosen base so a
    # libdir mismatch can't break configure.
    if (SENTRY_NATIVE_LIBDIR)
        set(_sentry_base ${SENTRY_NATIVE_LIBDIR})
    else ()
        set(_sentry_base ${CMAKE_INSTALL_PREFIX})
    endif ()

    set(sentry_DIR "")
    foreach (_suffix
            ${CMAKE_INSTALL_LIBDIR}/cmake/sentry
            lib/cmake/sentry
            lib64/cmake/sentry
            cmake/sentry)
        if (EXISTS ${_sentry_base}/${_suffix}/sentry-config.cmake)
            set(sentry_DIR ${_sentry_base}/${_suffix})
            break ()
        endif ()
    endforeach ()

    if (sentry_DIR)
        message(STATUS "Found libsentry: ${sentry_DIR}/sentry-config.cmake")
    else ()
        message(FATAL_ERROR
                "sentry-config.cmake not found under ${_sentry_base} "
                "(searched */cmake/sentry). Set SENTRY_NATIVE_LIBDIR to the "
                "sentry-native staging prefix.")
    endif ()


    if (CRASHPAD_BINARY_DIR)
        if (NOT EXISTS ${CRASHPAD_BINARY_DIR}/crashpad_handler)
            message(FATAL_ERROR "${CRASHPAD_BINARY_DIR}/crashpad_handler does not exist")
        else ()
            message(STATUS "Using crashpad_handler at specified directory: ${CRASHPAD_BINARY_DIR}")
        endif ()
    else ()
        if (EXISTS ${CMAKE_INSTALL_PREFIX}/bin/crashpad_handler)
            message(STATUS "Defaulting to system crashpad_handler at ${CMAKE_INSTALL_PREFIX}")
            set(CRASHPAD_BINARY_DIR ${CMAKE_INSTALL_PREFIX}/bin)
        else ()
            message(FATAL_ERROR "System crashpad_handler not found at ${CMAKE_INSTALL_PREFIX}, please set CRASHPAD_BINARY_DIR")
        endif()
    endif()
    
    find_package(sentry REQUIRED)
    string(TIMESTAMP BUILD_VER "%y%m%d")
else()
    message(STATUS "Crash Handler .......... Disabled")
endif ()

#
# watchdog
#
option(BUILD_WATCHDOG "Build Watchdog" OFF)
if (BUILD_WATCHDOG)
    option(BUILD_SYSTEMD_WATCHDOG "Build systemd Watchdog" OFF)
    if (BUILD_SYSTEMD_WATCHDOG)
        find_package(PkgConfig)
        pkg_check_modules(libsystemd REQUIRED IMPORTED_TARGET libsystemd)
        add_compile_definitions(BUILD_SYSTEMD_WATCHDOG)
    endif ()
endif ()

#
# Accessibility semantics tree
#
# When enabled, Flutter semantics updates are mirrored into an in-process
# accessibility tree and a JSON snapshot is written under the user's config
# directory. Off by default: it is a diagnostic feature and the snapshot file
# path is derived from the environment.
option(BUILD_ACCESSIBILITY "Build accessibility semantics tree" OFF)
if (BUILD_ACCESSIBILITY)
    MESSAGE(STATUS "Accessibility ........... Enabled")
else ()
    MESSAGE(STATUS "Accessibility ........... Disabled")
endif ()

#
# Static linking
#
option(ENABLE_STATIC_LINK "Link stdlib with static libs" OFF)

#
# Docs
#
option(BUILD_DOCS "Build documentation" OFF)
MESSAGE(STATUS "Build Documentation .... ${BUILD_DOCS}")

#
# Unit Tests
#
option(BUILD_UNIT_TESTS "Build Unit Tests" OFF)
MESSAGE(STATUS "Build Unit Tests ....... ${BUILD_UNIT_TESTS}")
option(UNIT_TEST_SAVE_GOLDENS "Generate Golden Images" OFF)
MESSAGE(STATUS "Generate Golden Images.. ${UNIT_TEST_SAVE_GOLDENS}")

#
# Sanitizers
#
find_package(Sanitizers)

#
# Executable Name
#
if (NOT EXE_OUTPUT_NAME)
    set(EXE_OUTPUT_NAME "homescreen")
endif ()
