#
# Copyright 2023 Toyota Connected North America
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

set(PLUGINS)

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

option(BUILD_PLUGIN_PACKAGE_INFO "Include PackageInfo Plugin" OFF)
if (BUILD_PLUGIN_PACKAGE_INFO)
    ENABLE_PLUGIN(package_info)
endif ()

option(BUILD_PLUGIN_COMP_SURF "Include Compositor Surface Plugin" OFF)
if (BUILD_PLUGIN_COMP_SURF)
    ENABLE_PLUGIN(comp_surf)
endif ()

option(BUILD_PLUGIN_COMP_REGION "Include Compositor Region Plugin" OFF)
if (BUILD_PLUGIN_COMP_REGION)
    ENABLE_PLUGIN(comp_region)
endif ()

if (BUILD_BACKEND_WAYLAND_EGL)
    option(BUILD_PLUGIN_OPENGL_TEXTURE "Includes OpenGL Texture Plugin" OFF)
    if (BUILD_PLUGIN_OPENGL_TEXTURE)
        ENABLE_PLUGIN(opengl_texture)
    endif ()
endif ()

option(BUILD_PLUGIN_GO_ROUTER "Includes Go Router Plugin" ON)
if (BUILD_PLUGIN_GO_ROUTER)
    ENABLE_PLUGIN(go_router)
endif ()

option(BUILD_VIEW_LAYER_PLAYGROUND "Layer Playground support" OFF)
if (BUILD_VIEW_LAYER_PLAYGROUND)
    set(ENABLE_VIEW_LAYER_PLAYGROUND)
endif ()

option(BUILD_PLUGIN_SECURE_STORAGE "Includes Flutter Secure Storage" OFF)
if (BUILD_PLUGIN_SECURE_STORAGE)
    ENABLE_PLUGIN(secure_storage)
endif ()

option(BUILD_PLUGIN_INTEGRATION_TEST "Included Flutter Integration Test support" OFF)
if (BUILD_PLUGIN_INTEGRATION_TEST)
    ENABLE_PLUGIN(integration_test)
endif ()

option(BUILD_PLUGIN_GOOGLE_SIGN_IN "Include Google Sign In manager" OFF)
if (BUILD_PLUGIN_GOOGLE_SIGN_IN)
    ENABLE_PLUGIN(google_sign_in)
endif ()

option(BUILD_PLUGIN_FILE_SELECTOR "Include File Selector plugin" OFF)
if (BUILD_PLUGIN_FILE_SELECTOR)
    ENABLE_PLUGIN(file_selector)
endif ()

option(BUILD_PLUGIN_AUDIOPLAYERS_LINUX "Include Audioplayers Linux pPlugin" OFF)
if (BUILD_PLUGIN_AUDIOPLAYERS_LINUX)
    ENABLE_PLUGIN(audioplayers_linux)
endif ()

option(BUILD_PLUGIN_URL_LAUNCHER "Includes URL Launcher Plugin" OFF)
if (BUILD_PLUGIN_URL_LAUNCHER)
    ENABLE_PLUGIN(url_launcher)
endif ()

option(BUILD_PLUGIN_CLOUD_FIRESTORE "Plugin Cloud Firestore" OFF)
if (BUILD_PLUGIN_CLOUD_FIRESTORE)
    ENABLE_PLUGIN(cloud_firestore)
endif ()

option(BUILD_PLUGIN_FIREBASE_AUTH "Plugin Firebase Auth" OFF)
if (BUILD_PLUGIN_FIREBASE_AUTH)
    ENABLE_PLUGIN(firebase_auth)
endif ()

option(BUILD_PLUGIN_FIREBASE_STORAGE "Plugin Firebase Storage" OFF)
if (BUILD_PLUGIN_FIREBASE_STORAGE)
    ENABLE_PLUGIN(firebase_storage)
endif ()

if (BUILD_PLUGIN_CLOUD_FIRESTORE OR BUILD_PLUGIN_FIREBASE_AUTH OR BUILD_PLUGIN_FIREBASE_STORAGE)
    option(BUILD_PLUGIN_FIREBASE_CORE "Plugin Firebase Core" ON)
    if (BUILD_PLUGIN_FIREBASE_CORE)
        ENABLE_PLUGIN(firebase_core)
    endif ()
    include(firebase)
endif ()


option(BUILD_PLUGIN_FILAMENT_VIEW "Include Filament View plugin" OFF)
if (BUILD_PLUGIN_FILAMENT_VIEW)
    ENABLE_PLUGIN(filament_view)
endif ()

option(BUILD_PLUGIN_LAYER_PLAYGROUND_VIEW "Include Layer Playground View plugin" OFF)
if (BUILD_PLUGIN_LAYER_PLAYGROUND_VIEW)
    ENABLE_PLUGIN(layer_playground_view)
endif ()

option(BUILD_PLUGIN_DESKTOP_WINDOW_LINUX "Includes Desktop Window Linux Plugin" OFF)
if (BUILD_PLUGIN_DESKTOP_WINDOW_LINUX)
    ENABLE_PLUGIN(desktop_window_linux)
endif ()

option(BUILD_PLUGIN_WEBVIEW_FLUTTER "Includes WebView Flutter View Plugin" OFF)
if (BUILD_PLUGIN_WEBVIEW_FLUTTER)
    ENABLE_PLUGIN(webview_flutter)
endif ()

option(BUILD_PLUGIN_VIDEO_PLAYER_LINUX "Include Video Player plugin" OFF)
if (BUILD_PLUGIN_VIDEO_PLAYER_LINUX)
    ENABLE_PLUGIN(video_player_linux)
endif ()

option(BUILD_PLUGIN_CAMERA "Include Camera plugin" OFF)
if (BUILD_PLUGIN_CAMERA)
    ENABLE_PLUGIN(camera)
endif ()

option(BUILD_PLUGIN_PDF "Include PDF plugin" OFF)
if (BUILD_PLUGIN_PDF)
    ENABLE_PLUGIN(pdf)
endif ()

ENABLE_PLUGIN(common)