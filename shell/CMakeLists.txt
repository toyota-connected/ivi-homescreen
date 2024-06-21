#
# Copyright 2020-2024 Toyota Connected North America
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

add_executable(${PROJECT_NAME}
        app.cc
        configuration/configuration.cc
        engine.cc
        libflutter_engine.cc
        main.cc
        timer.cc
        view/flutter_view.cc
        wayland/display.cc
        wayland/window.cc
        task_runner.cc
)
set_target_properties(${PROJECT_NAME}
        PROPERTIES OUTPUT_NAME "${EXE_OUTPUT_NAME}"
)


if (BUILD_BACKEND_WAYLAND_VULKAN)
    target_sources(${PROJECT_NAME} PRIVATE backend/wayland_vulkan/wayland_vulkan.cc)
endif ()

if (BUILD_BACKEND_WAYLAND_EGL)
    target_sources(${PROJECT_NAME} PRIVATE
            backend/wayland_egl/wayland_egl.cc
            backend/wayland_egl/egl.cc
            backend/gl_process_resolver.cc
    )
endif ()

if (BUILD_BACKEND_HEADLESS_EGL)
    target_sources(${PROJECT_NAME} PRIVATE
            backend/headless/headless.cc
            backend/headless/osmesa.cc
            backend/gl_process_resolver.cc
    )
endif ()

if (ENABLE_DLT)
    target_sources(${PROJECT_NAME} PRIVATE logging/dlt/dlt.cc logging/dlt/libdlt.cc)
endif ()

if (BUILD_CRASH_HANDLER)
    target_sources(${PROJECT_NAME} PRIVATE crash_handler.cc)
endif ()

target_compile_definitions(${PROJECT_NAME}
        PRIVATE
        HAVE_STRCHRNUL
        EGL_NO_X11
        MESA_EGL_NO_X11_HEADERS
        HAVE_MEMFD_CREATE
        WL_EGL_PLATFORM
        FILAMENT_SUPPORTS_WAYLAND
)

target_include_directories(${PROJECT_NAME}
        PRIVATE
        ..
        logging
        ${PROJECT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${GST_INCLUDE_DIRS}
        ${GST_VIDEO_INCLUDE_DIRS}
        ${AVFORMAT_INCLUDE_DIRS}
)

if (BUILD_BACKEND_WAYLAND_EGL)
    target_link_libraries(${PROJECT_NAME} PRIVATE EGL GLESv2)
endif ()
if (BUILD_BACKEND_HEADLESS_EGL)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::OSMESA)
endif ()
if (BUILD_CRASH_HANDLER)
    target_link_libraries(${PROJECT_NAME} PRIVATE sentry::sentry PkgConfig::UNWIND)
endif ()

target_link_libraries(${PROJECT_NAME} PRIVATE
        asio
        flutter
        spdlog
        Threads::Threads
        wayland-gen
        dl
        ${GST_LIBRARIES}
        ${GST_VIDEO_LIBRARIES}
        ${AVFORMAT_LIBRARIES}
        platform_homescreen
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
    if (ENABLE_CLANG_STATIC_LINK)
        target_link_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fuse-ld=lld -l:libc++.a -l:libc++abi.a -static-libgcc -lc -lm -v>)
    else ()
        target_link_options(${PROJECT_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fuse-ld=lld -lc++ -lc++abi -lgcc -lc -lm -v>)
    endif ()
endif ()

install(TARGETS ${PROJECT_NAME} DESTINATION bin)
