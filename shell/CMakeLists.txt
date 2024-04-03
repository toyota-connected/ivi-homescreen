#
# Copyright 2020 Toyota Connected North America
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

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_THREAD_PREFER_PTHREAD ON)
include(FindThreads)

include(wayland)

add_executable(${PROJECT_NAME}
        app.cc
        configuration/configuration.cc
        engine.cc
        json_utils.cc
        libflutter_engine.cc
        main.cc
        platform_channel.cc
        textures/texture.cc
        timer.cc
        view/flutter_view.cc
        wayland/display.cc
        wayland/window.cc
        logging/logging.cc
        task_runner.cc
)

include(plugins)
include(textures)

if (ENABLE_DLT)
    target_sources(${PROJECT_NAME} PRIVATE logging/dlt/dlt.cc logging/dlt/libdlt.cc)
endif ()

if (BUILD_BACKEND_WAYLAND_VULKAN)
    target_sources(${PROJECT_NAME} PRIVATE backend/wayland_vulkan.cc)
endif ()

if (BUILD_PLUGIN_COMP_SURF)
    target_sources(${PROJECT_NAME} PRIVATE view/compositor_surface.cc)
endif ()

if (BUILD_BACKEND_WAYLAND_EGL)
    target_sources(${PROJECT_NAME} PRIVATE
            backend/wayland_egl.cc
            backend/egl.cc
            backend/gl_process_resolver.cc
    )
endif ()

if (BUILD_CRASH_HANDLER)
    target_sources(${PROJECT_NAME} PRIVATE crash_handler.cc)
endif ()

if (BUILD_PLUGIN_GOOGLE_SIGN_IN)
    target_sources(${PROJECT_NAME} PRIVATE curl_client/curl_client.cc)
endif ()

if (BUILD_PLUGIN_AUDIO_PLAYERS)
    target_sources(${PROJECT_NAME} PRIVATE plugins/audio_players/audio_player.cc)
    target_sources(${PROJECT_NAME} PRIVATE plugins/audio_players/audio_players_registry.cc)
endif ()

if (BUILD_PLUGIN_PLATFORM_VIEWS OR BUILD_PLUGIN_LAYER_PLAYGROUND)
    target_sources(${PROJECT_NAME} PRIVATE
            plugins/platform_views/platform_view.cc
            plugins/platform_views/platform_views_registry.cc
    )
endif ()

if (BUILD_PLUGIN_FIREBASE_CORE)
    target_sources(${PROJECT_NAME} PRIVATE
            plugins/firebase_core/firebase_core_plugin.cpp
            plugins/firebase_core/firebase_core_plugin_c_api.cpp
            plugins/firebase_core/firebase_core.cc
    )
    target_include_directories(${PROJECT_NAME} PRIVATE ${FIREBASE_PLUGIN_INCLUDE_DIRS})
    target_link_directories(${PROJECT_NAME} PRIVATE ${FIREBASE_PLUGIN_LIB_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE firebase_app flatbuffers z)
endif ()

target_compile_definitions(${PROJECT_NAME}
        PRIVATE
        HAVE_STRCHRNUL
        EGL_NO_X11
        MESA_EGL_NO_X11_HEADERS
        LINUX
        HAVE_MEMFD_CREATE
        PATH_PREFIX="${CMAKE_INSTALL_PREFIX}"
        EXE_OUTPUT_NAME="${EXE_OUTPUT_NAME}"
        WL_EGL_PLATFORM
        FILAMENT_SUPPORTS_WAYLAND
        ASIO_STANDALONE
        ASIO_HEADER_ONLY
        ASIO_NO_TYPEID
        ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS
        ASIO_NO_DEPRECATED
)
set_target_properties(${PROJECT_NAME}
        PROPERTIES OUTPUT_NAME "${EXE_OUTPUT_NAME}"
)

target_include_directories(${PROJECT_NAME}
        PRIVATE
        ..
        logging
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${GST_INCLUDE_DIRS}
        ${GST_VIDEO_INCLUDE_DIRS}
        ${LIBCURL_INCLUDE_DIRS}
        ${AVFORMAT_INCLUDE_DIRS}
        ${PLUGIN_SECURE_STORAGE_INCLUDE_DIRS}
        ../third_party/spdlog-1.12.0/include
        ../third_party/asio-1-28-1/asio/include
)

if (BUILD_BACKEND_WAYLAND_EGL)
    target_link_libraries(${PROJECT_NAME} PRIVATE EGL GLESv2)
endif ()
if (BUILD_PLUGIN_COMP_SURF)
    target_link_libraries(${PROJECT_NAME} PRIVATE EGL)
endif ()
if (BUILD_BACKEND_WAYLAND_VULKAN)
    target_link_libraries(${PROJECT_NAME} PRIVATE bluevk)
endif ()
if (BUILD_CRASH_HANDLER)
    target_link_libraries(${PROJECT_NAME} PRIVATE sentry::sentry PkgConfig::UNWIND)
endif ()

target_link_libraries(${PROJECT_NAME} PRIVATE
        flutter
        wayland-gen
        Threads::Threads
        dl
        ${GST_LIBRARIES}
        ${GST_VIDEO_LIBRARIES}
        ${LIBCURL_LIBRARIES}
        ${AVFORMAT_LIBRARIES}
        ${PLUGIN_SECURE_STORAGE_LINK_LIBRARIES}
)

target_link_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_BINARY_DIR}
)

add_sanitizers(${PROJECT_NAME})

if (IPO_SUPPORT_RESULT)
    set_property(TARGET ${PROJECT_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${PROJECT_NAME} PRIVATE ${CONTEXT_COMPILE_OPTIONS})
    target_link_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fuse-ld=lld -lc++ -lc++abi -lc -lm -v>)
endif ()

install(TARGETS ${PROJECT_NAME} DESTINATION bin)
