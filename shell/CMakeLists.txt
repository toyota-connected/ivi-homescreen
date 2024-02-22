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

add_executable(homescreen
        app.cc
        configuration/configuration.cc
        engine.cc
        libflutter_engine.cc
        main.cc
        textures/texture.cc
        timer.cc
        view/flutter_view.cc
        wayland/display.cc
        wayland/window.cc
        logging/logging.cc
        task_runner.cc
)

include(textures)

if (ENABLE_DLT)
    target_sources(homescreen PRIVATE logging/dlt/dlt.cc logging/dlt/libdlt.cc)
endif ()

if (BUILD_BACKEND_WAYLAND_VULKAN)
    target_sources(homescreen PRIVATE backend/wayland_vulkan.cc)
endif ()

if (BUILD_PLUGIN_COMP_SURF)
    target_sources(homescreen PRIVATE view/compositor_surface.cc)
endif ()

if (BUILD_BACKEND_WAYLAND_EGL)
    target_sources(homescreen PRIVATE
            backend/wayland_egl.cc
            backend/egl.cc
            backend/gl_process_resolver.cc
    )
endif ()

if (BUILD_BACKEND_HEADLESS)
    target_sources(homescreen PRIVATE
            backend/headless.cc
            backend/osmesa.cc
            backend/osmesa_process_resolver.cc
            backend/gl_process_resolver.cc
    )
endif ()

if (BUILD_CRASH_HANDLER)
    target_sources(homescreen PRIVATE crash_handler.cc)
endif ()

if (BUILD_PLUGIN_GOOGLE_SIGN_IN)
    # target_sources(homescreen PRIVATE curl_client/curl_client.cc)
endif ()

target_compile_definitions(homescreen
        PRIVATE
        HAVE_STRCHRNUL
        EGL_NO_X11
        MESA_EGL_NO_X11_HEADERS
        HAVE_MEMFD_CREATE
        PATH_PREFIX="${CMAKE_INSTALL_PREFIX}"
        WL_EGL_PLATFORM
        FILAMENT_SUPPORTS_WAYLAND
)

target_include_directories(homescreen
        PRIVATE
        ..
        logging
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${GST_INCLUDE_DIRS}
        ${GST_VIDEO_INCLUDE_DIRS}
        ${AVFORMAT_INCLUDE_DIRS}
        ../third_party/asio-1-28-1/asio/include
)

if (BUILD_BACKEND_WAYLAND_EGL)
    target_link_libraries(homescreen PRIVATE EGL GLESv2)
endif ()
if (BUILD_BACKEND_HEADLESS)
    target_link_libraries(homescreen PRIVATE OSMesa EGL GLESv2)
endif ()
if (BUILD_PLUGIN_COMP_SURF)
    target_link_libraries(homescreen PRIVATE EGL)
endif ()
if (BUILD_BACKEND_WAYLAND_VULKAN)
    target_link_libraries(homescreen PRIVATE bluevk)
endif ()
if (BUILD_CRASH_HANDLER)
    target_link_libraries(homescreen PRIVATE sentry::sentry PkgConfig::UNWIND)
endif ()

target_link_libraries(homescreen PRIVATE
        spdlog
        flutter
        wayland-gen
        Threads::Threads
        dl
        ${GST_LIBRARIES}
        ${GST_VIDEO_LIBRARIES}
        ${AVFORMAT_LIBRARIES}
        platform_homescreen
)

target_link_directories(homescreen PRIVATE
        ${CMAKE_BINARY_DIR}
)

add_sanitizers(homescreen)

if (IPO_SUPPORT_RESULT)
    set_property(TARGET homescreen PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(homescreen PRIVATE ${CONTEXT_COMPILE_OPTIONS})
    target_link_options(homescreen PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fuse-ld=lld -l:libc++.a -l:libc++abi.a -static-libgcc -lc -lm -v>)
endif ()

install(TARGETS homescreen DESTINATION bin)
