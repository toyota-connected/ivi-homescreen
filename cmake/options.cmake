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

# variables
set(THIRD_PARTY_DIR ${CMAKE_SOURCE_DIR}/third_party)

# EGL Surface Options
if (NOT BUILD_EGL_CONTEXT_VERSION_MAJOR)
    set(BUILD_EGL_CONTEXT_VERSION_MAJOR "3" CACHE STRING "Valid options are: 1, 2, 3." FORCE)
    message(STATUS "EGL_CONTEXT_VERSION_MAJOR not set, defaulting to 3.")
endif()
add_compile_definitions(BUILD_EGL_CONTEXT_VERSION_MAJOR=${BUILD_EGL_CONTEXT_VERSION_MAJOR})

option(BUILD_EGL_OPENGL_ES3 "Build with EGL_OPENGL_ES3_BIT set" ON)
option(BUILD_EGL_OPENGL_ES2 "Build with EGL_OPENGL_ES2_BIT set" OFF)
if(BUILD_EGL_OPENGL_ES3)
    add_compile_definitions(BUILD_EGL_OPENGL_ES3)
elseif(BUILD_EGL_OPENGL_ES2)
    add_compile_definitions(BUILD_EGL_OPENGL_ES2)
endif()

option(BUILD_EGL_TRANSPARENCY "Build with EGL Transparency Enabled" ON)
if(BUILD_EGL_TRANSPARENCY)
    add_compile_definitions(BUILD_EGL_ENABLE_TRANSPARENCY)
endif()
