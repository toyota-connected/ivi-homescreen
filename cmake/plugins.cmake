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

#
# Plugin Configuration
#
set(PLUGINS)

option(BUILD_PLUGIN_ISOLATE "Include Isolate Plugin" ON)
if (BUILD_PLUGIN_ISOLATE)
    ENABLE_PLUGIN(isolate)
endif ()

option(BUILD_PLUGIN_RESTORATION "Include Restoration Plugin" ON)
if (BUILD_PLUGIN_RESTORATION)
    ENABLE_PLUGIN(restoration)
endif ()

option(BUILD_PLUGIN_PLATFORM "Include Platform Plugin" ON)
if (BUILD_PLUGIN_PLATFORM)
    ENABLE_PLUGIN(platform)
endif ()

option(BUILD_PLUGIN_MOUSE_CURSOR "Include Mouse Cursor Plugin" ON)
if (BUILD_PLUGIN_MOUSE_CURSOR)
    ENABLE_PLUGIN(mouse_cursor)
endif ()

option(BUILD_PLUGIN_TEXT_INPUT "Includes Text Input Plugin" ON)
if (BUILD_PLUGIN_TEXT_INPUT)
    ENABLE_PLUGIN(text_input)
endif ()

option(BUILD_PLUGIN_URL_LAUNCHER "Includes URL Launcher Plugin" ON)
if (BUILD_PLUGIN_URL_LAUNCHER)
    ENABLE_PLUGIN(url_launcher)
endif ()

option(BUILD_PLUGIN_PACKAGE_INFO "Include PackageInfo Plugin" ON)
if (BUILD_PLUGIN_PACKAGE_INFO)
    ENABLE_PLUGIN(package_info)
endif ()

option(BUILD_PLUGIN_OPENGL_TEXTURE "Includes OpenGL Texture Plugin" ON)
if (BUILD_PLUGIN_OPENGL_TEXTURE)
    ENABLE_PLUGIN(opengl_texture)
endif ()

option(BUILD_PLUGIN_NAVIGATION "Includes Navigation Plugin" OFF)
if (BUILD_PLUGIN_NAVIGATION)
    ENABLE_PLUGIN(navigation)
endif ()

option(BUILD_PLUGIN_ACCESSIBILITY "Includes Accessibility Plugin" OFF)
if (BUILD_PLUGIN_ACCESSIBILITY)
    ENABLE_PLUGIN(accessibility)
endif ()

option(BUILD_PLUGIN_CONNECTIVITY "Includes Connectivity Plugin" OFF)
if (BUILD_PLUGIN_CONNECTIVITY)
    ENABLE_PLUGIN(connectivity)
endif ()

option(BUILD_PLUGIN_PLATFORM_VIEW "Includes PlatformView Plugin" OFF)
if (BUILD_PLUGIN_PLATFORM_VIEW)
    ENABLE_PLUGIN(platform_views)
endif ()

message(STATUS "Plugin Config .......... ${PLUGINS}")
