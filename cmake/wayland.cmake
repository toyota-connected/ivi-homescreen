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

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-egl wayland-cursor xkbcommon)
set(MIN_PROTOCOL_VER 1.13)
if (BUILD_BACKEND_WAYLAND_DRM)
    set(MIN_PROTOCOL_VER 1.22)
endif ()
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=${MIN_PROTOCOL_VER})
pkg_get_variable(WAYLAND_PROTOCOLS_BASE wayland-protocols pkgdatadir)

find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner REQUIRED)

macro(wayland_generate protocol_file output_file)
    add_custom_command(OUTPUT ${output_file}.h
            COMMAND ${WAYLAND_SCANNER_EXECUTABLE} client-header < ${protocol_file} > ${output_file}.h
            DEPENDS ${protocol_file})
    list(APPEND WAYLAND_PROTOCOL_SOURCES ${output_file}.h)

    add_custom_command(OUTPUT ${output_file}.c
            COMMAND ${WAYLAND_SCANNER_EXECUTABLE} private-code < ${protocol_file} > ${output_file}.c
            DEPENDS ${protocol_file})
    list(APPEND WAYLAND_PROTOCOL_SOURCES ${output_file}.c)
endmacro()

set(WAYLAND_PROTOCOL_SOURCES)
wayland_generate(
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/agl/protocol/agl-shell.xml
        ${CMAKE_CURRENT_BINARY_DIR}/agl-shell-client-protocol)
wayland_generate(
        ${WAYLAND_PROTOCOLS_BASE}/stable/xdg-shell/xdg-shell.xml
        ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-client-protocol)
if (BUILD_BACKEND_WAYLAND_DRM)
    wayland_generate(
            ${WAYLAND_PROTOCOLS_BASE}/staging/drm-lease/drm-lease-v1.xml
            ${CMAKE_CURRENT_BINARY_DIR}/drm-lease-v1-client-protocol)
endif ()
