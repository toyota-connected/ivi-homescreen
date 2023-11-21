#
# Copyright 2020-2022 Toyota Connected North America
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

if (BUILD_BACKEND_WAYLAND_EGL)
    option(BUILD_PLUGIN_GSTREAMER_EGL "Include GStreamer Plugin" OFF)
    if (BUILD_PLUGIN_GSTREAMER_EGL)
        ENABLE_PLUGIN(gstreamer_egl)
        pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.4)
        pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-1.0>=1.4)
        pkg_check_modules(AVFORMAT libavformat<59.16.100)
        if (AVFORMAT_FOUND)
            message(STATUS "AVFORMAT without Const FOUND")
        else ()
            pkg_check_modules(AVFORMAT REQUIRED libavformat>=59.16.100)
            message(STATUS "AVFORMAT with Const FOUND")
            add_definitions(-DAVFORMAT_WITH_CONST)
        endif ()
    endif ()
endif ()

option(BUILD_PLUGIN_TEXT_INPUT "Includes Text Input Plugin" ON)
if (BUILD_PLUGIN_TEXT_INPUT)
    ENABLE_PLUGIN(text_input)
endif ()

option(BUILD_PLUGIN_KEY_EVENT "Includes Key Event Plugin" ON)
if (BUILD_PLUGIN_KEY_EVENT)
    ENABLE_PLUGIN(key_event)
endif ()

option(BUILD_PLUGIN_URL_LAUNCHER "Includes URL Launcher Plugin" ON)
if (BUILD_PLUGIN_URL_LAUNCHER)
    ENABLE_PLUGIN(url_launcher)
endif ()

option(BUILD_PLUGIN_PACKAGE_INFO "Include PackageInfo Plugin" ON)
if (BUILD_PLUGIN_PACKAGE_INFO)
    ENABLE_PLUGIN(package_info)
endif ()

option(BUILD_PLUGIN_COMP_SURF "Include Compositor Surface Plugin" ON)
if (BUILD_PLUGIN_COMP_SURF)
    ENABLE_PLUGIN(comp_surf)
endif ()

option(BUILD_PLUGIN_COMP_REGION "Include Compositor Region Plugin" ON)
if (BUILD_PLUGIN_COMP_REGION)
    ENABLE_PLUGIN(comp_region)
endif ()

if (BUILD_BACKEND_WAYLAND_EGL)
    option(BUILD_PLUGIN_OPENGL_TEXTURE "Includes OpenGL Texture Plugin" ON)
    if (BUILD_PLUGIN_OPENGL_TEXTURE)
        ENABLE_PLUGIN(opengl_texture)
    endif ()
endif ()

option(BUILD_PLUGIN_GO_ROUTER "Includes Go Router Plugin" ON)
if (BUILD_PLUGIN_GO_ROUTER)
    ENABLE_PLUGIN(go_router)
endif ()

option(BUILD_PLUGIN_ACCESSIBILITY "Includes Accessibility Plugin" ON)
if (BUILD_PLUGIN_ACCESSIBILITY)
    ENABLE_PLUGIN(accessibility)
endif ()

option(BUILD_PLUGIN_PLATFORM_VIEW "Includes PlatformView Plugin" OFF)
if (BUILD_PLUGIN_PLATFORM_VIEW)
    ENABLE_PLUGIN(platform_views)
endif ()

option(BUILD_PLUGIN_DESKTOP_WINDOW "Includes Desktop Window Plugin" ON)
if (BUILD_PLUGIN_DESKTOP_WINDOW)
    ENABLE_PLUGIN(desktop_window)
endif ()

option(BUILD_PLUGIN_SECURE_STORAGE "Includes Flutter Secure Storage" OFF)
if (BUILD_PLUGIN_SECURE_STORAGE)
    ENABLE_PLUGIN(secure_storage)
    pkg_check_modules(PLUGIN_SECURE_STORAGE REQUIRED libsecret-1)
endif ()

option(BUILD_PLUGIN_INTEGRATION_TEST "Included Flutter Integration Test support" OFF)
if (BUILD_PLUGIN_INTEGRATION_TEST)
    ENABLE_PLUGIN(integration_test)
endif ()

option(BUILD_PLUGIN_LOGGING "Includes Logging Plugin" ON)
if (BUILD_PLUGIN_LOGGING)
    ENABLE_PLUGIN(logging)
endif ()

option(BUILD_PLUGIN_KEYBOARD_MANAGER "Include Keyboard Manager" ON)
if (BUILD_PLUGIN_KEYBOARD_MANAGER)
    ENABLE_PLUGIN(keyboard_manager)
endif ()

option(BUILD_PLUGIN_GOOGLE_SIGN_IN "Include Google Sign In manager" OFF)
if (BUILD_PLUGIN_GOOGLE_SIGN_IN)
    ENABLE_PLUGIN(google_sign_in)
    pkg_check_modules(LIBCURL REQUIRED libcurl)
endif ()

option(BUILD_PLUGIN_FILE_SELECTOR "Include File Selector plugin" OFF)
if (BUILD_PLUGIN_FILE_SELECTOR)
    ENABLE_PLUGIN(file_selector)
endif ()

option(BUILD_PLUGIN_AUDIO_PLAYERS "Include Audio Players plugin" OFF)
if (BUILD_PLUGIN_AUDIO_PLAYERS)
    ENABLE_PLUGIN(audio_players)
    pkg_check_modules(GST REQUIRED gstreamer-1.0>=1.4)
endif ()

message(STATUS "Plugin Config .......... ${PLUGINS}")
