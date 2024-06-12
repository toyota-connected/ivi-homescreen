#
# Copyright 2021-2024 Toyota Connected North America
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

pkg_check_modules(WAYLAND_EGL REQUIRED IMPORTED_TARGET wayland-egl)

add_library(plugin_nav_render_view STATIC
        nav_render_view_plugin_c_api.cc
        nav_render_surface.cc
        nav_render_texture.cc
        libnav_render.cc
)

target_include_directories(plugin_nav_render_view PRIVATE include ${PROJECT_BINARY_DIR})

target_link_libraries(plugin_nav_render_view PUBLIC
        flutter
        platform_homescreen
        PkgConfig::WAYLAND_EGL
        EGL
)
