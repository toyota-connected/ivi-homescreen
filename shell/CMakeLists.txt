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
        watchdog.cc
        task_runner.cc
        accessibility/accessibility_tree.cc
)

# Wayland-specific TUs:
#   - wayland/display.cc + wayland/window.cc            — Display/Window
#   - view/present_layer_sequencer.cc                   — wl_subsurface
#                                                         layer-ordering
# All call sites in the main shell are already
# #if-gated on a Wayland backend. Skipping these for non-Wayland
# binaries drops the libwayland-client + libwayland-cursor link/runtime
# deps; drm-kms-egl and software builds become truly Wayland-free.
if (BUILD_BACKEND_WAYLAND_EGL OR BUILD_BACKEND_WAYLAND_VULKAN OR
    BUILD_BACKEND_HEADLESS_EGL)
    target_sources(${PROJECT_NAME} PRIVATE
            wayland/display.cc
            wayland/window.cc
            view/present_layer_sequencer.cc
    )
endif ()
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

if (BUILD_BACKEND_SOFTWARE)
    target_sources(${PROJECT_NAME} PRIVATE
            backend/software/file_sink.cc
            backend/software/sink_factory.cc
            backend/software/software_backend.cc
            display/software_display.cc
    )
    if (BUILD_SOFTWARE_SINK_DRM)
        target_sources(${PROJECT_NAME} PRIVATE
                backend/software/drm_dumb_sink.cc
        )
        target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::DRM_DUMB)
    endif ()
    if (BUILD_SOFTWARE_SINK_FBDEV)
        target_sources(${PROJECT_NAME} PRIVATE
                backend/software/fbdev_sink.cc
        )
    endif ()
    if (BUILD_SOFTWARE_INPUT_LIBINPUT)
        target_sources(${PROJECT_NAME} PRIVATE
                backend/software/input/software_seat.cc
        )
        target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::SW_LIBINPUT)
    endif ()
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
        # LayerScene-driven non-framed present path. Sources compile only
        # when USE_DRM_SCENE=ON; the OFF build keeps the in-tree allocator
        # path and never references drm::scene symbols. scene_layer_source_vk
        # is built dormant: it provides the interface for an Impeller-on-Linux
        # backing-store wrapper that lights up once the Vulkan renderer config
        # is wired through; nothing references it at runtime today.
        if (USE_DRM_SCENE)
            target_sources(${PROJECT_NAME} PRIVATE
                    backend/drm_kms_egl/scene_layer_source_gl.cc
                    backend/drm_kms_egl/scene_layer_source_vk.cc
            )
        endif ()
    endif ()
    target_link_libraries(${PROJECT_NAME} PRIVATE
            drm-cxx::drm-cxx
            PkgConfig::DRM
            PkgConfig::GBM
            PkgConfig::LIBINPUT
            PkgConfig::UDEV
            # EGL/GLESv2 are listed ahead of platform_homescreen (which
            # references glDeleteTextures et al.), so --as-needed would drop
            # libGLESv2 before its reference is seen. Force them DT_NEEDED.
            -Wl,--no-as-needed
            EGL
            GLESv2
            -Wl,--as-needed
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
        dl
        ${GST_LIBRARIES}
        ${GST_VIDEO_LIBRARIES}
        ${AVFORMAT_LIBRARIES}
        platform_homescreen
)

# Propagate wayland-gen's include dirs unconditionally (config/common.h
# pulls <waypp/waypp.h> regardless of backend for the AGL/XDG client
# compile defines) but link the static library only on Wayland builds
# so non-Wayland binaries don't drag in libwayland-client /
# libwayland-cursor as runtime deps.
target_include_directories(${PROJECT_NAME} PRIVATE
        $<TARGET_PROPERTY:wayland-gen,INTERFACE_INCLUDE_DIRECTORIES>
)
if (BUILD_BACKEND_WAYLAND_EGL OR BUILD_BACKEND_WAYLAND_VULKAN OR
    BUILD_BACKEND_HEADLESS_EGL)
    target_link_libraries(${PROJECT_NAME} PRIVATE wayland-gen)
endif ()

target_link_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_BINARY_DIR}
)

add_sanitizers(${PROJECT_NAME})

install(TARGETS ${PROJECT_NAME} DESTINATION bin)
