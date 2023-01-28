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
# for c++ compatibility
#
# std::unordered_map is faster than std::map, but it is supported after c++11.
option(BUILD_UNORDERED_MAP "Build std::unordered_map instead of std::map" ON)
if (BUILD_UNORDERED_MAP)
    add_compile_definitions(BUILD_UNORDERED_MAP)
endif ()

#
# backend selection
#
option(BUILD_BACKEND_WAYLAND_EGL "Build Backend for EGL" ON)
if (BUILD_BACKEND_WAYLAND_EGL)
    add_compile_definitions(BUILD_BACKEND_WAYLAND_EGL)
    option(BUILD_EGL_TRANSPARENCY "Build with EGL Transparency Enabled" ON)
    if (BUILD_EGL_TRANSPARENCY)
        add_compile_definitions(BUILD_EGL_ENABLE_TRANSPARENCY)
    endif ()
else ()
    option(BUILD_BACKEND_WAYLAND_VULKAN "Build Backend for Vulkan" ON)
    if (BUILD_BACKEND_WAYLAND_VULKAN)
        add_compile_definitions(BUILD_BACKEND_WAYLAND_VULKAN)
    endif ()
endif ()

option(BUILD_BACKEND_WAYLAND_DRM "Build Backend Wayland DRM" OFF)
if (BUILD_BACKEND_WAYLAND_DRM)
    add_compile_definitions(BUILD_BACKEND_WAYLAND_DRM)
endif ()
