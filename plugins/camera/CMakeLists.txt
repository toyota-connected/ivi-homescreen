#
# Copyright 2023-2024 Toyota Connected North America
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

pkg_check_modules(CAMERA REQUIRED libcamera)
include(FindJPEG)

add_library(plugin_camera STATIC
        camera_plugin_c_api.cc
        camera_plugin.cc
        messages.cc
        camera_context.cc
)

target_include_directories(plugin_camera PUBLIC
        include
        ${CAMERA_INCLUDE_DIRS}
        ${JPEG_INCLUDE_DIRS}
)

target_link_libraries(plugin_camera PUBLIC
        flutter
        platform_homescreen
        plugin_common
        ${CAMERA_LINK_LIBRARIES}
        ${JPEG_LIBRARIES}
)

if (JPEG_FOUND)
    target_compile_definitions(plugin_camera PUBLIC ENABLE_JPEG)
endif ()

add_sanitizers(plugin_camera)

if (IPO_SUPPORT_RESULT)
    set_property(TARGET plugin_camera PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()
