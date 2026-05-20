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
        view/present_layer_sequencer.cc
        watchdog.cc
        wayland/display.cc
        wayland/window.cc
        task_runner.cc
        accessibility/accessibility_tree.cc
)
set_target_properties(${PROJECT_NAME}
        PROPERTIES OUTPUT_NAME "${EXE_OUTPUT_NAME}"
)


if (BUILD_BACKEND_WAYLAND_VULKAN)
    target_sources(${PROJECT_NAME} PRIVATE backend/wayland_vulkan/wayland_vulkan.cc)
    if (BUILD_COMPOSITOR)
        target_sources(${PROJECT_NAME} PRIVATE
                backend/wayland_vulkan/vulkan_backing_store.cc
        )
    endif ()
endif ()

if (BUILD_BACKEND_WAYLAND_EGL)
    target_sources(${PROJECT_NAME} PRIVATE
            backend/wayland_egl/wayland_egl.cc
            backend/wayland_egl/egl.cc
            backend/gl_process_resolver.cc
    )
    if (BUILD_COMPOSITOR)
        target_sources(${PROJECT_NAME} PRIVATE
                backend/wayland_egl/egl_backing_store.cc
                backend/wayland_egl/gl_caps.cc
                backend/wayland_egl/gl_compositor.cc
        )
    endif ()
endif ()

if (BUILD_BACKEND_HEADLESS_EGL)
    target_sources(${PROJECT_NAME} PRIVATE
            backend/headless/headless.cc
            backend/headless/osmesa.cc
            backend/gl_process_resolver.cc
    )
endif ()

if (BUILD_BACKEND_DRM_KMS_EGL)
    target_sources(${PROJECT_NAME} PRIVATE
            backend/drm_kms_egl/drm_backend.cc
            backend/drm_kms_egl/drm_session.cc
            backend/drm_kms_egl/driver_probe.cc
            backend/gl_process_resolver.cc
            display/drm_display.cc
            input/drm_seat.cc
    )
    # DRM HW cursor depends on drm-cxx's cursor module, which gates on
    # libxcursor. Probe for libxcursor here so the homescreen build
    # tracks drm-cxx's optionality: present → compile drm_cursor.cc
    # and define HAVE_DRM_CURSOR=1; absent → skip the cursor wire-up.
    pkg_check_modules(XCURSOR xcursor)
    if (XCURSOR_FOUND)
        target_sources(${PROJECT_NAME} PRIVATE
                backend/drm_kms_egl/drm_cursor.cc
        )
        target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_DRM_CURSOR=1)
    else ()
        message(STATUS "libxcursor not found; DRM HW cursor disabled")
    endif ()
    # SIGUSR1-driven snapshot diagnostic. Depends on drm-cxx's capture
    # module, which gates on Blend2D. We mirror that probe so the
    # homescreen build skips drm_capture.cc when Blend2D is absent —
    # otherwise drm::capture::snapshot / write_png would unresolved at
    # link time. find_package matches drm-cxx's own gating.
    find_package(blend2d CONFIG QUIET)
    if (blend2d_FOUND)
        target_sources(${PROJECT_NAME} PRIVATE
                backend/drm_kms_egl/drm_capture.cc
        )
        target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_DRM_CAPTURE=1)
    else ()
        message(STATUS "blend2d not found; SIGUSR1 snapshot diagnostic disabled")
    endif ()
    if (BUILD_COMPOSITOR)
        # gl_caps and gl_compositor live under wayland_egl/ but are
        # backend-agnostic GL helpers. Share the TUs rather than forking.
        # egl_backing_store is NOT needed — the DRM path uses GbmBackingStore.
        target_sources(${PROJECT_NAME} PRIVATE
                backend/drm_kms_egl/drm_compositor.cc
                backend/wayland_egl/gl_caps.cc
                backend/wayland_egl/gl_compositor.cc
        )
    endif ()
    target_link_libraries(${PROJECT_NAME} PRIVATE
            drm-cxx::drm-cxx
            PkgConfig::DRM
            PkgConfig::GBM
            PkgConfig::LIBINPUT
            PkgConfig::UDEV
            EGL
            GLESv2
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
        accessibility
        ${PROJECT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${GST_INCLUDE_DIRS}
        ${GST_VIDEO_INCLUDE_DIRS}
        ${AVFORMAT_INCLUDE_DIRS}
)

if (BUILD_BACKEND_WAYLAND_EGL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EGL egl glesv2 IMPORTED_TARGET REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::EGL)
endif ()
if (BUILD_BACKEND_WAYLAND_VULKAN)
    target_link_libraries(${PROJECT_NAME} PRIVATE vulkan_headers)
endif ()
if (BUILD_BACKEND_HEADLESS_EGL)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::OSMESA)
endif ()
if (BUILD_CRASH_HANDLER)
    target_link_libraries(${PROJECT_NAME} PRIVATE sentry::sentry PkgConfig::UNWIND)
endif ()
if (BUILD_SYSTEMD_WATCHDOG)
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::libsystemd)
endif ()

target_link_libraries(${PROJECT_NAME} PRIVATE
        asio
        flutter
        cxxopts::cxxopts
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

install(TARGETS ${PROJECT_NAME} DESTINATION bin)
