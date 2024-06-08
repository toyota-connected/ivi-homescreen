
#
# Copyright 2020-2024 Toyota Connected North America
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

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

include_directories(${PROJECT_BINARY_DIR})

macro(ENABLE_PLUGIN plugin)

    string(TOUPPER ${plugin} ucase_plugin)

    set(ENABLE_PLUGIN_${ucase_plugin} ON)

    if (${plugin} MATCHES ".*_view")
        message(STATUS "View: ${plugin}")
    else ()
        message(STATUS "Plugin: ${plugin}")
    endif ()

    add_subdirectory(${plugin})

    target_compile_definitions(plugin_${plugin} PUBLIC ENABLE_PLUGIN_${ucase_plugin})

    add_sanitizers(plugin_${plugin})

    if (IPO_SUPPORT_RESULT)
        set_property(TARGET plugin_${plugin} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif ()


    target_link_libraries(plugins INTERFACE plugin_${plugin})

    target_compile_definitions(plugins INTERFACE ENABLE_PLUGIN_${ucase_plugin})

    #print_target_properties(plugin_${plugin})

endmacro(ENABLE_PLUGIN)

macro(PLUGIN_OPTION plugin description default)

    string(TOUPPER ${plugin} ucase_plugin)

    option(BUILD_PLUGIN_${ucase_plugin} ${description} ${default})

    set(ENABLE_PLUGIN_${ucase_plugin} OFF)
    if (BUILD_PLUGIN_${ucase_plugin})
        ENABLE_PLUGIN(${plugin})
    endif ()

endmacro(PLUGIN_OPTION)

# target used at top level
add_library(plugins INTERFACE)

# Plugins

add_subdirectory(common)

PLUGIN_OPTION(audioplayers_linux "Include Audioplayers Linux plugin" OFF)

PLUGIN_OPTION(camera "Include Camera plugin" OFF)

PLUGIN_OPTION(cloud_firestore "Plugin Cloud Firestore" OFF)

PLUGIN_OPTION(desktop_window_linux "Includes Desktop Window Linux Plugin" OFF)

PLUGIN_OPTION(file_selector "Include File Selector plugin" OFF)

PLUGIN_OPTION(firebase_auth "Plugin Firebase Auth" OFF)

PLUGIN_OPTION(firebase_storage "Plugin Firebase Storage" OFF)

PLUGIN_OPTION(go_router "Includes Go Router Plugin" ON)

PLUGIN_OPTION(google_sign_in "Include Google Sign In manager" OFF)

PLUGIN_OPTION(integration_test "Included Flutter Integration Test support" OFF)

PLUGIN_OPTION(pdf "Include PDF plugin" OFF)

PLUGIN_OPTION(secure_storage "Includes Flutter Secure Storage" OFF)

PLUGIN_OPTION(url_launcher "Includes URL Launcher Plugin" OFF)

PLUGIN_OPTION(video_player_linux "Include Video Player plugin" OFF)


if (BUILD_PLUGIN_CLOUD_FIRESTORE OR BUILD_PLUGIN_FIREBASE_AUTH OR BUILD_PLUGIN_FIREBASE_STORAGE)
    PLUGIN_OPTION(firebase_core "Plugin Firebase Core" ON)
    include(firebase)
endif ()

# TODO create source file for view

# Platform View Plugins

PLUGIN_OPTION(filament_view "Include Filament View plugin" OFF)

PLUGIN_OPTION(layer_playground_view "Include Layer Playground View plugin" OFF)

PLUGIN_OPTION(nav_render_view "Include Navigation Render View plugin" OFF)

PLUGIN_OPTION(webview_flutter_view "Includes WebView View Plugin" OFF)


#
# Configuration File
#
configure_file(plugins_config.h.in ${PROJECT_BINARY_DIR}/plugins_config.h)
