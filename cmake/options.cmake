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

    # Locate sentry-native. When the caller pins a staging prefix
    # (SENTRY_NATIVE_LIBDIR), honor it first so a build that stages a specific
    # sentry-native stays deterministic. Otherwise prefer CMake config mode: it
    # honors CMAKE_PREFIX_PATH / CMAKE_FIND_ROOT_PATH, so a sentry staged into a
    # cross overlay (e.g. an emb augment) is found with no extra plumbing.
    if (NOT SENTRY_NATIVE_LIBDIR)
        find_package(sentry QUIET CONFIG)
    endif ()

    if (NOT sentry_FOUND)
        # Fallback: search a caller-provided staging prefix. sentry-native
        # installs its CMake package config under the GNUInstallDirs libdir of
        # that prefix -- typically lib/cmake/sentry, but lib64 on some distros
        # and a flat cmake/sentry on older layouts. Hardcoding a single suffix
        # breaks whenever ${CMAKE_INSTALL_LIBDIR} here differs from the one
        # sentry-native installed with (or is empty because GNUInstallDirs had
        # not been included yet). Search the likely suffixes under the chosen
        # base so a libdir mismatch can't break configure.
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

        if (NOT sentry_DIR)
            message(FATAL_ERROR
                    "sentry-config.cmake not found via CMAKE_PREFIX_PATH or "
                    "under ${_sentry_base} (searched */cmake/sentry). Set "
                    "SENTRY_NATIVE_LIBDIR to the sentry-native staging prefix.")
        endif ()

        find_package(sentry REQUIRED)
    endif ()

    message(STATUS "Found libsentry: ${sentry_DIR}")

    # crashpad_handler resolution. CRASHPAD_BINARY_DIR is a HOST/build-tree
    # location, validated here only as a configure-time sanity gate -- it is NOT
    # the path baked into the binary (see CRASHPAD_RUNTIME_PATH below), so a
    # cross build may set it to the overlay/build tree (host path) freely.
    if (CRASHPAD_BINARY_DIR)
        if (NOT EXISTS ${CRASHPAD_BINARY_DIR}/crashpad_handler)
            message(FATAL_ERROR "${CRASHPAD_BINARY_DIR}/crashpad_handler does not exist")
        endif ()
        message(STATUS "Using crashpad_handler at specified directory: ${CRASHPAD_BINARY_DIR}")
    elseif (CMAKE_CROSSCOMPILING)
        # Cross build: crashpad_handler is packaged onto the target rootfs by the
        # cross tooling, not consumed here, so there is no host binary to check.
        message(STATUS "Cross build: crashpad_handler expected on target")
    elseif (EXISTS ${CMAKE_INSTALL_PREFIX}/bin/crashpad_handler)
        message(STATUS "Defaulting to system crashpad_handler at ${CMAKE_INSTALL_PREFIX}")
        set(CRASHPAD_BINARY_DIR ${CMAKE_INSTALL_PREFIX}/bin)
    else ()
        # Native/overlay-staged build: sentry-native's crashpad_handler ships in
        # an augment overlay (e.g. emb stages it under <overlay>/usr/bin). Search
        # CMAKE_PREFIX_PATH's bin dirs, symmetric with the find_package(sentry)
        # CONFIG lookup above, so no per-build CRASHPAD_BINARY_DIR is needed.
        find_program(_crashpad_handler crashpad_handler
                PATHS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES usr/bin bin)
        if (_crashpad_handler)
            get_filename_component(CRASHPAD_BINARY_DIR "${_crashpad_handler}" DIRECTORY)
            message(STATUS "Found crashpad_handler at ${CRASHPAD_BINARY_DIR}")
        else ()
            message(FATAL_ERROR "crashpad_handler not found at ${CMAKE_INSTALL_PREFIX}/bin or on CMAKE_PREFIX_PATH; set CRASHPAD_BINARY_DIR")
        endif ()
    endif ()

    # Absolute path to crashpad_handler on the DEPLOYED target -- this is what is
    # baked into the binary and handed to sentry at runtime. For a native build
    # it is the resolved host location; for a cross/staged build it is the
    # on-target install path (the cross tooling packages the handler there).
    # Override with -DCRASHPAD_RUNTIME_PATH=... when the target differs.
    if (NOT DEFINED CRASHPAD_RUNTIME_PATH OR CRASHPAD_RUNTIME_PATH STREQUAL "")
        if (CMAKE_CROSSCOMPILING)
            set(CRASHPAD_RUNTIME_PATH "/usr/bin/crashpad_handler")
        else ()
            set(CRASHPAD_RUNTIME_PATH "${CRASHPAD_BINARY_DIR}/crashpad_handler")
        endif ()
    endif ()
    set(CRASHPAD_RUNTIME_PATH "${CRASHPAD_RUNTIME_PATH}" CACHE STRING
            "Absolute path to crashpad_handler on the deployed target" FORCE)
    message(STATUS "Crashpad handler runtime path: ${CRASHPAD_RUNTIME_PATH}")
    string(TIMESTAMP BUILD_VER "%y%m%d")
else()
    message(STATUS "Crash Handler .......... Disabled")
endif ()

option(INTEGRATION_TEST_CRASH_HANDLER "Enable crash handler integration test" OFF)
if (INTEGRATION_TEST_CRASH_HANDLER)
    message(STATUS "Crash Handler Integration Test ... Enabled")
else()
    message(STATUS "Crash Handler Integration Test ... Disabled")
endif ()

#
# watchdog
#
option(BUILD_WATCHDOG "Build Watchdog" OFF)
if (BUILD_WATCHDOG)
    message(STATUS "Watchdog ............... Enabled")
    option(BUILD_SYSTEMD_WATCHDOG "Build systemd Watchdog" OFF)
    if (BUILD_SYSTEMD_WATCHDOG)
        message(STATUS "Systemd Watchdog ....... Enabled")
        find_package(PkgConfig)
        pkg_check_modules(libsystemd REQUIRED IMPORTED_TARGET libsystemd)
        add_compile_definitions(BUILD_SYSTEMD_WATCHDOG)

        option(INTEGRATION_TEST_SYSTEMD_WATCHDOG "Enable systemd watchdog integration test (requires BUILD_SYSTEMD_WATCHDOG=ON)" OFF)
        if (INTEGRATION_TEST_SYSTEMD_WATCHDOG)
            message(STATUS "Systemd Watchdog Integration Test ... Enabled")
            add_compile_definitions(INTEGRATION_TEST_SYSTEMD_WATCHDOG)
        else()
            message(STATUS "Systemd Watchdog Integration Test ... Disabled")
        endif()
    else()
        message(STATUS "Systemd Watchdog ....... Disabled")
    endif()
else()
    message(STATUS "Watchdog ............... Disabled")
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
# Derived backend flags
#
# Declared last so every BUILD_BACKEND_* / BUILD_SOFTWARE_SINK_* above is
# already defined. These name the distinctions the gates actually care about,
# which are NOT the same as "is some Wayland backend on":
#
#   IVI_WAYLAND_SURFACE_BACKENDS — backends that own a wl_surface and take
#       input/focus from the compositor. These are the only consumers of
#       wayland-cursor, xkbcommon, and the surface protocol set (xdg, IME,
#       viewporter, dmabuf, fractional-scale, syncobj).
#   IVI_WAYLAND_ANY — anything that opens a wl_display connection at all.
#       wayland-leased-drm connects, but only to negotiate a DRM lease: it has
#       no wl_surface, and takes input from evdev. It therefore needs the
#       scanner + wayland-client and nothing else, so a lease-only build must
#       gate on this rather than on IVI_WAYLAND_SURFACE_BACKENDS.
#
# The leased renderer tiers reuse the existing renderer stacks rather than
# adding parallel options — the lease only changes how the DRM fd is acquired,
# not how frames are produced.
#
if (BUILD_BACKEND_WAYLAND_EGL OR BUILD_BACKEND_WAYLAND_VULKAN)
    set(IVI_WAYLAND_SURFACE_BACKENDS TRUE)
else ()
    set(IVI_WAYLAND_SURFACE_BACKENDS FALSE)
endif ()

if (IVI_WAYLAND_SURFACE_BACKENDS OR BUILD_BACKEND_WAYLAND_LEASED_DRM)
    set(IVI_WAYLAND_ANY TRUE)
else ()
    set(IVI_WAYLAND_ANY FALSE)
endif ()

# Per-tier availability for the three wayland-leased-drm registry keys.
set(IVI_LEASED_DRM_EGL FALSE)
set(IVI_LEASED_DRM_VULKAN FALSE)
set(IVI_LEASED_DRM_SOFTWARE FALSE)
if (BUILD_BACKEND_WAYLAND_LEASED_DRM)
    if (BUILD_BACKEND_DRM_KMS_EGL)
        set(IVI_LEASED_DRM_EGL TRUE)
    endif ()
    if (BUILD_BACKEND_DRM_KMS_VULKAN)
        set(IVI_LEASED_DRM_VULKAN TRUE)
    endif ()
    if (BUILD_BACKEND_SOFTWARE AND BUILD_SOFTWARE_SINK_DRM)
        set(IVI_LEASED_DRM_SOFTWARE TRUE)
    endif ()
    if (NOT IVI_LEASED_DRM_EGL AND NOT IVI_LEASED_DRM_VULKAN AND
        NOT IVI_LEASED_DRM_SOFTWARE)
        message(FATAL_ERROR
                "BUILD_BACKEND_WAYLAND_LEASED_DRM=ON but no renderer tier is "
                "available — a lease acquires a DRM fd, it does not render, so "
                "at least one renderer stack must be enabled to scan out on it. "
                "Enable one of: BUILD_BACKEND_DRM_KMS_EGL "
                "(wayland-leased-drm-egl), BUILD_BACKEND_DRM_KMS_VULKAN "
                "(wayland-leased-drm-vulkan), or BUILD_BACKEND_SOFTWARE + "
                "BUILD_SOFTWARE_SINK_DRM (wayland-leased-drm-software).")
    endif ()
    # All three tiers are wired. ResolveKeyForConfig refuses a leased key that
    # is not registered rather than falling back to an unleased backend, so a
    # tier missing its renderer stack fails loudly instead of silently running
    # something else.
    if (IVI_LEASED_DRM_EGL)
        message(STATUS "  wayland-leased-drm-egl ........ registered")
    endif ()
    if (IVI_LEASED_DRM_SOFTWARE)
        message(STATUS "  wayland-leased-drm-software ... registered")
    endif ()
    if (IVI_LEASED_DRM_VULKAN)
        message(STATUS "  wayland-leased-drm-vulkan ..... registered")
    endif ()
endif ()

message(STATUS "Wayland (surface) ...... ${IVI_WAYLAND_SURFACE_BACKENDS}")
message(STATUS "Wayland (any conn.) .... ${IVI_WAYLAND_ANY}")
message(STATUS "Leased DRM tiers ....... egl=${IVI_LEASED_DRM_EGL} vulkan=${IVI_LEASED_DRM_VULKAN} software=${IVI_LEASED_DRM_SOFTWARE}")

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
