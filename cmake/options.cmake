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
# Plugin source directory lists
#
# In-tree plugin dirs (set by the workspace or CI to point at the
# checked-out ivi-homescreen-plugins paths). Absolute paths only.
set(HOMESCREEN_PLUGINS_STATIC  "" CACHE STRING
    "Semicolon-separated absolute paths of in-tree plugin source dirs to link statically into homescreen")
set(HOMESCREEN_PLUGINS_DYNAMIC "" CACHE STRING
    "Semicolon-separated absolute paths of in-tree plugin source dirs to build as shared libraries")

# Out-of-tree plugin dirs (e.g. ivi-homescreen-plugins-example or any
# downstream private plugin repo). Concatenated onto the in-tree lists
# at configure time so workspace orchestration can add external plugins
# without re-listing the in-tree set.
set(HOMESCREEN_EXTERNAL_PLUGINS_STATIC  "" CACHE STRING
    "Additional out-of-tree static plugin source dirs (absolute paths)")
set(HOMESCREEN_EXTERNAL_PLUGINS_DYNAMIC "" CACHE STRING
    "Additional out-of-tree dynamic plugin source dirs (absolute paths)")

if (HOMESCREEN_PLUGINS_STATIC OR HOMESCREEN_EXTERNAL_PLUGINS_STATIC)
    MESSAGE(STATUS "Static Plugins ......... ${HOMESCREEN_PLUGINS_STATIC} ${HOMESCREEN_EXTERNAL_PLUGINS_STATIC}")
else ()
    MESSAGE(STATUS "Static Plugins ......... None")
endif ()

# Conditional plugin dir groups: each *_LOAD flag (ON/OFF) gates whether the
# corresponding *_PLUGIN_DIRS paths are appended to HOMESCREEN_PLUGINS_STATIC.
foreach(_grp FIREBASE RIVE CAMERA CAMERA_PIPEWIRE PDF WEBRTC WEBVIEW NAV_RENDER)
    if(${_grp}_LOAD AND ${_grp}_PLUGIN_DIRS)
        list(APPEND HOMESCREEN_PLUGINS_STATIC ${${_grp}_PLUGIN_DIRS})
        MESSAGE(STATUS "${_grp} plugins .......... Enabled")
    endif()
endforeach()

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
# DRM
#
option(BUILD_BACKEND_DRM "Build DRM backend" OFF)
option(BUILD_BACKEND_WAYLAND_LEASED_DRM "Build Wayland Leased DRM backend" OFF)

#
# Headless
#
option(BUILD_BACKEND_HEADLESS_EGL "Build Headless EGL Backend" OFF)
if (BUILD_BACKEND_HEADLESS_EGL)
    find_package(PkgConfig)
    pkg_check_modules(OSMESA osmesa glesv2 egl IMPORTED_TARGET REQUIRED)
endif ()

option(DEBUG_PLATFORM_MESSAGES "Debug platform messages" OFF)

#
# Crash Handler
#
option(BUILD_CRASH_HANDLER "Build Crash Handler" OFF)
if (BUILD_CRASH_HANDLER)
    if (SENTRY_NATIVE_LIBDIR)
        if (NOT EXISTS ${SENTRY_NATIVE_LIBDIR}/cmake/sentry/sentry-config.cmake)
            message(FATAL_ERROR "${SENTRY_NATIVE_LIBDIR}/cmake/sentry/sentry-config.cmake does not exist")
        else ()
            message(STATUS "Found libsentry at specified directory: ${SENTRY_NATIVE_LIBDIR}/cmake/sentry/sentry-config.cmake")
            set(sentry_DIR ${SENTRY_NATIVE_LIBDIR}/cmake/sentry)
        endif ()
    else ()
        if (EXISTS ${CMAKE_INSTALL_PREFIX}/lib/cmake/sentry/sentry-config.cmake)
            message(STATUS "Found sentry at default location: ${CMAKE_INSTALL_PREFIX}/lib/cmake/sentry/sentry-config.cmake")
            set(sentry_DIR {CMAKE_INSTALL_PREFIX}/lib/cmake/sentry)
        else ()
            message(FATAL_ERROR "Sentry could not be found at ${CMAKE_INSTALL_PREFIX}/lib/cmake/sentry/sentry-config.cmake, please set SENTRY_NATIVE_LIBDIR")
        endif()
    endif ()

    
    if (CRASHPAD_BINARY_DIR)
        if (NOT EXISTS ${CRASHPAD_BINARY_DIR}/crashpad_handler)
            message(FATAL_ERROR "${CRASHPAD_BINARY_DIR}/crashpad_handler does not exist")
        else()
            message(STATUS "Using crashpad_handler at specified directory: ${CRASHPAD_BINARY_DIR}")
        endif()
    else ()
        if (EXISTS ${CMAKE_INSTALL_PREFIX}/bin/crashpad_handler)
            message(STATUS "Defaulting to system crashpad_handler at ${CMAKE_INSTALL_PREFIX}")
            set(CRASHPAD_BINARY_DIR ${CMAKE_INSTALL_PREFIX}/bin)
        else ()
            message(FATAL_ERROR "System crashpad_handler not found at ${CMAKE_INSTALL_PREFIX}, please set CRASHPAD_BINARY_DIR")
        endif()
    endif()

    if (NOT CRASH_HANDLER_DSN)
        message(STATUS "Sentry DSN not set, use environment variable SENTRY_DSN to direct coredumps")
    endif ()
    
    find_package(sentry REQUIRED)
    find_package(PkgConfig)
    pkg_check_modules(UNWIND REQUIRED IMPORTED_TARGET libunwind)
    string(TIMESTAMP BUILD_VER "%y%m%d")
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
