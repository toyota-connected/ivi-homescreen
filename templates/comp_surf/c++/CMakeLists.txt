#
# Copyright 2022 Toyota Connected North America
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
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug, Release, or MinSizeRel." FORCE)
    message(STATUS "CMAKE_BUILD_TYPE not set, defaulting to Release.")
endif ()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

if (NOT BUILD_NUMBER)
    set(BUILD_NUMBER 0)
endif ()

project(comp_surf_cxx
        VERSION "1.0.${BUILD_NUMBER}"
        DESCRIPTION "Compositor Surface C++ Template"
        LANGUAGES CXX C
        )

message(STATUS "Project ................ ${PROJECT_NAME}")
message(STATUS "Version ................ ${PROJECT_VERSION}")
message(STATUS "Generator .............. ${CMAKE_GENERATOR}")
message(STATUS "Build Type ............. ${CMAKE_BUILD_TYPE}")

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(options)
include(flags)
include(target_arch)

add_subdirectory(third_party)


#
# System packages
#

#find_package(PkgConfig REQUIRED)
#pkg_check_modules(GLES glesv2 egl REQUIRED)
#pkg_check_modules(WAYLAND REQUIRED wayland-egl)

set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)


#
# Library
#

set(API_PUBLIC_HEADERS include/${PROJECT_NAME}/${PROJECT_NAME}.h)


add_library(${PROJECT_NAME} SHARED
        src/main.cpp
        src/context.h
        src/context.cpp
        )

target_compile_definitions(${PROJECT_NAME} PRIVATE
        EGL_NO_X11
        MESA_EGL_NO_X11_HEADERS
        )

target_include_directories(${PROJECT_NAME}
        PUBLIC
        include
        PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/src
        ${CMAKE_CURRENT_LIST_DIR}/include
        ${CMAKE_BINARY_DIR})

target_link_libraries(${PROJECT_NAME} PRIVATE
        ${GLES_LINK_LIBRARIES}
        Threads::Threads
        ${CMAKE_DL_LIBS}
        )

set_target_properties(${PROJECT_NAME}
        PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MINOR}
        )

add_custom_command(TARGET ${PROJECT_NAME}
        POST_BUILD
        COMMAND
        ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}
        )

#
# Optimizations
#

cmake_policy(SET CMP0069 NEW)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_support_result OUTPUT ipo_support_output)
if (ipo_support_result)
    set_property(TARGET ${PROJECT_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
else ()
    message(WARNING "IPO is not supported: ${ipo_support_output}")
endif ()

#
# Install
#

include(GNUInstallDirs)

target_compile_definitions(${PROJECT_NAME}
        PRIVATE
        ASSET_DIRECTORY="${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/assets"
        )

message(STATUS "install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "staging prefix: ${CMAKE_STAGING_PREFIX}")

install(TARGETS ${PROJECT_NAME})
install(FILES ${API_PUBLIC_HEADERS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${PROJECT_NAME})
install(DIRECTORY assets DESTINATION ${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME})

configure_file(${PROJECT_NAME}.pc.in ${PROJECT_NAME}.pc)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig/)


#
# Packaging
#

if (NOT CMAKE_CROSSCOMPILING)
    include(packaging)
endif ()
