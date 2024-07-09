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

if (NOT PLUGIN_DIR)
    set(PLUGIN_DIR ${CMAKE_SOURCE_DIR}/plugins)
endif ()

if (NOT DISABLE_PLUGINS AND EXISTS ${PLUGIN_DIR})
    MESSAGE(STATUS "Plugins ................ Enabled")
    set(ENABLE_PLUGINS ON)
elseif (DISABLE_PLUGINS OR NOT EXISTS ${PLUGIN_DIR})
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
    if (NOT EXISTS ${SENTRY_NATIVE_LIBDIR}/cmake/sentry/sentry-config.cmake)
        message(FATAL_ERROR "${SENTRY_NATIVE_LIBDIR}/cmake/sentry/sentry-config.cmake does not exist")
    endif ()
    set(sentry_DIR ${SENTRY_NATIVE_LIBDIR}/cmake/sentry)
    find_package(sentry REQUIRED)
    find_package(PkgConfig)
    pkg_check_modules(UNWIND REQUIRED IMPORTED_TARGET libunwind)
endif ()

#
# Static linking
#
option(ENABLE_CLANG_STATIC_LINK "Link libc++, libc++abi, and gcc as static libs when building with clang" OFF)

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
if(NOT EXE_OUTPUT_NAME)
    set(EXE_OUTPUT_NAME "homescreen")
endif ()
