#
# Copyright 2020 Toyota Connected North America
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

cmake_minimum_required(VERSION 3.10.2)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug, Release, RelWithDebInfo, or MinSizeRel." FORCE)
    message(STATUS "CMAKE_BUILD_TYPE not set, defaulting to Release.")
endif ()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/third_party/sanitizers-cmake/cmake)

if (NOT BUILD_NUMBER)
    set(BUILD_NUMBER 0)
endif ()

project(ivi-homescreen
        VERSION "1.0.${BUILD_NUMBER}"
        DESCRIPTION "IVI Flutter Shell"
        LANGUAGES CXX C
)

include(git)
include(options)

# Backend selection is no longer mutually exclusive: every BUILD_BACKEND_* may
# be ON together and all selected backends link into one binary. The active
# backend is resolved at runtime via backend::ShellRegistry (see
# shell/backend/register_backends.cc + main.cc) rather than chosen at compile
# time. Each backend contributes its own renderer config (Backend::GetRenderConfig
# is virtual: kOpenGL / kVulkan / kSoftware) and its own App loop mode, so the
# former three #if/#elif chains and the configure-time FATAL_ERROR guards that
# enforced a single choice have both been removed.
#
# At least one backend must still be selected, otherwise the registry is empty
# and main() has nothing to run.
if (NOT (BUILD_BACKEND_WAYLAND_EGL OR BUILD_BACKEND_WAYLAND_VULKAN OR
         BUILD_BACKEND_DRM_KMS_EGL OR BUILD_BACKEND_DRM_KMS_VULKAN OR
         BUILD_BACKEND_SOFTWARE))
    message(FATAL_ERROR
            "No backend selected — enable at least one BUILD_BACKEND_* "
            "(WAYLAND_EGL, WAYLAND_VULKAN, DRM_KMS_EGL, DRM_KMS_VULKAN, "
            "SOFTWARE)")
endif ()

message(STATUS "Project ................ ${PROJECT_NAME}")
message(STATUS "Version ................ ${PROJECT_VERSION}")
message(STATUS "Generator .............. ${CMAKE_GENERATOR}")
message(STATUS "Build Type ............. ${CMAKE_BUILD_TYPE}")

include(compiler)
include(cxx_compat)

# drm_kms.cmake must run after compiler.cmake so the `toolchain` INTERFACE
# library (which carries -stdlib=libc++ on clang builds) exists when we
# add_subdirectory(drm-cxx). Otherwise drm-cxx compiles against libstdc++
# and link fails with mixed ABI symbols.
if (BUILD_BACKEND_DRM_KMS_EGL OR BUILD_BACKEND_DRM_KMS_VULKAN)
    include(drm_kms)
endif ()

# wayland_cxx.cmake (like drm_kms) must run after compiler.cmake so the
# `toolchain` INTERFACE library exists when we add_subdirectory the
# wayland-cxx-scanner subproject on clang/libc++ builds. Gated internally on the
# Wayland backends.
if (BUILD_BACKEND_WAYLAND_EGL OR BUILD_BACKEND_WAYLAND_VULKAN)
    include(wayland_cxx)
endif ()

# The software backend resolves XCursor themes via drm-cxx's loader sources
# built in-tree (no display deps); see cmake/drm_cxx_cursor_load.cmake. When a
# DRM backend is ALSO compiled in, it already links drm-cxx::drm-cxx — which
# contains the very same drm::cursor::{Theme,Cursor} objects — so building the
# standalone loader too would duplicate those symbols at link. In that case the
# software backend reuses drm-cxx::drm-cxx instead (see shell/CMakeLists.txt).
if (BUILD_BACKEND_SOFTWARE AND NOT (BUILD_BACKEND_DRM_KMS_EGL OR
        BUILD_BACKEND_DRM_KMS_VULKAN))
    include(drm_cxx_cursor_load)
endif ()

include(logging_helpers)

configure_file(cmake/config_common.h.in ${PROJECT_BINARY_DIR}/config/common.h)

#
# ihs_shared: C-ABI shared library for out-of-tree FFI plugins (logging,
# tracing, platform-view negotiation, config). See docs/PLUGIN_ABI.md.
#
add_subdirectory(shared)

#
# libraries
#
add_subdirectory(third_party)

add_subdirectory(shell/platform/homescreen)

#
# plugins
#
if (ENABLE_PLUGINS)
    add_subdirectory(${PLUGINS_DIR} ${PROJECT_BINARY_DIR}/plugins)
    foreach (EXT_PLUGINS_DIR ${EXT_PLUGINS_DIRS})
        if (EXISTS ${EXT_PLUGINS_DIR})
            add_subdirectory(${EXT_PLUGINS_DIR} ${PROJECT_BINARY_DIR}/ext_plugins)
        endif ()
    endforeach ()

    # Treat the vendored sdbus-c++ as a system library from our side (we don't
    # edit that project): re-publish its interface include dirs as SYSTEM so its
    # headers are -isystem for consumers and don't spam our build with warnings.
    foreach (_tgt sdbus-c++ sdbus-c++-objlib)
        if (TARGET ${_tgt})
            get_target_property(_sdbus_inc ${_tgt} INTERFACE_INCLUDE_DIRECTORIES)
            if (_sdbus_inc)
                set_target_properties(${_tgt} PROPERTIES
                        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_sdbus_inc}")
            endif ()
            # Also suppress warnings emitted while compiling sdbus-cpp's own
            # sources (e.g. -Wconversion in VTableUtils.c). System includes only
            # cover consumers, so quiet the vendored target's own build here.
            target_compile_options(${_tgt} PRIVATE -w)
        endif ()
    endforeach ()
endif ()

#
# executable
#
add_subdirectory(shell)

#print_target_properties(asio)
#print_target_properties(flutter)
#print_target_properties(plugin_url_launcher)
#print_target_properties(platform_homescreen)
#print_target_properties(homescreen)


add_subdirectory(docs)

#
# Unit Tests
#
if (BUILD_UNIT_TESTS)
    if (NOT DEFINED UNIT_TEST_APP_BUNDLE)
        set(UNIT_TEST_APP_BUNDLE "/home/root")
    endif ()
    if (UNIT_TEST_SAVE_GOLDENS)
        add_compile_definitions(SAVE_IMAGE_FOR_COMPARISON)
    endif ()
    enable_testing()
    add_subdirectory(test)
    install(DIRECTORY DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/test/unit_test/test_images_golden)
    install(DIRECTORY DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/test/unit_test/test_images)
endif ()

if (NOT CMAKE_CROSSCOMPILING)
    include(packaging)
endif ()
