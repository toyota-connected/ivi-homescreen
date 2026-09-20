# Copyright 2026 Toyota Connected North America
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

# Built out of tree, against a shell build directory -- the same way a real
# out-of-tree producer is built, and so a benchmark artifact never lands in the
# shell's own build.
#
#   cmake -B build -DIHS_BUILD_DIR=/path/to/shell/build \
#         [-DCMAKE_TOOLCHAIN_FILE=... for a cross build]
#   cmake --build build

cmake_minimum_required(VERSION 3.16)
project(pv_bench_producer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (NOT IHS_BUILD_DIR)
    message(FATAL_ERROR "set -DIHS_BUILD_DIR to a configured shell build dir")
endif ()

# ihs_export.h is generated, so the headers come from two roots.
get_filename_component(IHS_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../.."
        ABSOLUTE)
set(_ihs_shared_so "${IHS_BUILD_DIR}/shell/lib/libihs_shared.so")
if (NOT EXISTS "${_ihs_shared_so}")
    message(FATAL_ERROR "no libihs_shared.so in ${IHS_BUILD_DIR}; build the shell first")
endif ()

# pkg-config has no idea it is being cross-compiled: the toolchain file sets a
# sysroot for the compiler and for find_package, but not for this. Left alone it
# answers with the host's gbm and libdrm, and the first sign of it is a link
# error against x86_64 objects.
if (CMAKE_CROSSCOMPILING AND CMAKE_SYSROOT)
    file(GLOB _pc_dirs
            "${CMAKE_SYSROOT}/usr/lib/*/pkgconfig"
            "${CMAKE_SYSROOT}/usr/lib/pkgconfig"
            "${CMAKE_SYSROOT}/usr/share/pkgconfig")
    string(REPLACE ";" ":" _pc_path "${_pc_dirs}")
    set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
    set(ENV{PKG_CONFIG_LIBDIR} "${_pc_path}")
endif ()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)

# gbm_bo_create_with_modifiers2 (usage flags alongside modifiers) arrived in
# Mesa 21.1. Older stacks -- and vendor gbm implementations, which are often
# older still -- have only the usage-less form, so detect rather than assume:
# the alternative is a producer that will not compile for the targets most
# likely to need it.
include(CheckSymbolExists)
set(CMAKE_REQUIRED_INCLUDES ${GBM_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${GBM_LIBRARIES})
check_symbol_exists(gbm_bo_create_with_modifiers2 "gbm.h"
        HAVE_GBM_BO_CREATE_WITH_MODIFIERS2)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)

add_library(pv_bench_producer SHARED pv_bench_producer.cc)

if (HAVE_GBM_BO_CREATE_WITH_MODIFIERS2)
    target_compile_definitions(pv_bench_producer PRIVATE
            HAVE_GBM_BO_CREATE_WITH_MODIFIERS2)
endif ()

target_include_directories(pv_bench_producer PRIVATE
        ${IHS_SOURCE_DIR}/shared/include
        ${IHS_BUILD_DIR}/shared/include)

target_link_libraries(pv_bench_producer PRIVATE
        ${_ihs_shared_so}
        PkgConfig::GBM
        PkgConfig::LIBDRM)

find_package(Threads REQUIRED)
target_link_libraries(pv_bench_producer PRIVATE Threads::Threads)
