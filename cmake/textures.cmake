#
# Copyright 2020-2022 Toyota Connected North America
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

include(macros)

set(TEXTURES)

if (BUILD_BACKEND_WAYLAND_EGL)
    option(BUILD_TEXTURE_EGL "Include EGL Textures" ON)
    if (BUILD_TEXTURE_EGL)
        ENABLE_TEXTURE(egl)
    endif ()

    option(BUILD_TEXTURE_TEST_EGL "Includes Test Texture" OFF)
    if (BUILD_TEXTURE_TEST_EGL)
        ENABLE_TEXTURE(test_egl)
    endif ()

    option(BUILD_TEXTURE_NAVI_RENDER_EGL "Includes Navi Texture" ON)
    if (BUILD_TEXTURE_NAVI_RENDER_EGL)
        ENABLE_TEXTURE(navi_render_egl)
        option(BUILD_TEXTURE_NAVI_EGL_ROUTING "Includes Navi Routing" OFF)
        if (BUILD_TEXTURE_NAVI_EGL_ROUTING)
            add_compile_definitions(BUILD_TEXTURE_NAVI_EGL_ROUTING)
        endif ()
    endif ()

endif ()

message(STATUS "Texture Config ......... ${TEXTURES}")
