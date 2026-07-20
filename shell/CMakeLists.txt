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
        # Runtime backend registry + the compiled-in backend registration. The
        # registration TU gates each backend body behind its own BUILD_BACKEND_*
        # guard, so single-backend builds still pull in only one backend.
        backend/backend_registry.cc
        backend/register_backends.cc
        configuration/configuration.cc
        # Backend-agnostic output resolver (pure name/serial matching); the
        # per-backend output providers feed it via the IDisplay seam.
        display/output.cc
        # Binds each view to an output by name across backends (resolver +
        # provider); feeds the resolved connector / wl_output to the backend.
        display/output_manager.cc
        engine.cc
        libflutter_engine.cc
        main.cc
        main_loop_waker.cc
        profiling/frame_profile.cc
        profiling/motion_to_photon.cc
        # Backend-spanning vsync baton machinery (Wayland/DRM/software providers
        # inherit it). Source-agnostic, so always compiled.
        vsync/ivsync_provider.cc
        timer.cc
        view/flutter_view.cc
        watchdog.cc
        stats.cc
        task_runner.cc
)

if (BUILD_ACCESSIBILITY)
    target_sources(${PROJECT_NAME} PRIVATE
            accessibility/accessibility_tree.cc
            accessibility/semantics_translator.cc)
endif ()

# Wayland-specific TUs:
#   - wayland/display.cc + wayland/window.cc            — Display/Window
#   - view/present_layer_sequencer.cc                   — wl_subsurface
#                                                         layer-ordering
# All call sites in the main shell are already
# #if-gated on a Wayland backend. Skipping these for non-Wayland
# binaries drops the libwayland-client + libwayland-cursor link/runtime
# deps; drm-kms-egl and software builds become truly Wayland-free.
if (BUILD_BACKEND_WAYLAND_EGL OR BUILD_BACKEND_WAYLAND_VULKAN)
    target_sources(${PROJECT_NAME} PRIVATE
            wayland/display.cc
            wayland/window.cc
            view/present_layer_sequencer.cc
            # Compositor-protocol shell layer (xdg/agl/ivi/simple) + factory.
            wayland/shell/shells.cc
            # wp_presentation vsync provider (scanner presentation-time C++).
            vsync/wayland_vsync_provider.cc
            # zwp_input_timestamps_v1 provider (scanner input-timestamps C++).
            wayland/input_timestamps.cc
    )
    if (ENABLE_XDG_CLIENT OR ENABLE_AGL_SHELL_CLIENT)
        target_sources(${PROJECT_NAME} PRIVATE wayland/shell/xdg_shell.cc)
    endif ()
    if (ENABLE_AGL_SHELL_CLIENT)
        target_sources(${PROJECT_NAME} PRIVATE wayland/shell/agl_shell.cc)
    endif ()
    if (ENABLE_IVI_SHELL_CLIENT)
        target_sources(${PROJECT_NAME} PRIVATE wayland/shell/ivi_shell.cc)
    endif ()
    if (ENABLE_SIMPLE_SHELL_CLIENT)
        target_sources(${PROJECT_NAME} PRIVATE wayland/shell/simple_shell.cc)
    endif ()
endif ()
set_target_properties(${PROJECT_NAME}
        PROPERTIES OUTPUT_NAME "${EXE_OUTPUT_NAME}"
)


if (BUILD_BACKEND_WAYLAND_VULKAN)
    target_sources(${PROJECT_NAME} PRIVATE
            backend/wayland_vulkan/wayland_vulkan.cc
            backend/wayland_vulkan/vulkan_queue_interposer.cc
            backend/wayland_vulkan/wl_dmabuf_present.cc
            backend/wayland_vulkan/wl_explicit_sync.cc
            backend/wayland_vulkan/wl_layer_compositor.cc
    )
    # The dma-buf present path needs drm_fourcc.h (DRM_FORMAT_* constants), a
    # libdrm header. On a cross sysroot it lives under <libdrm/>, so the include
    # dir must come from pkg-config rather than the default search path.
    # GLOBAL so test/ can resolve it via TYPICAL_TEST_LINK_LIBS — see the note
    # in the wayland-leased-drm block below.
    if (NOT TARGET PkgConfig::DRM)
        pkg_check_modules(DRM REQUIRED IMPORTED_TARGET GLOBAL libdrm)
    endif ()
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::DRM)
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

# wayland-leased-drm: the drm-lease-v1 client. Renderer-agnostic — it only
# acquires the DRM fd; the egl/vulkan/software tiers then scan out on it, so
# this block is independent of which renderer stacks are enabled.
if (BUILD_BACKEND_WAYLAND_LEASED_DRM)
    target_sources(${PROJECT_NAME} PRIVATE
            backend/wayland_leased_drm/lease_client.cc
    )
    # drmGetDeviceNameFromFd2() resolves the lease/enumeration fd to a node path.
    # On a cross sysroot xf86drm.h lives under <libdrm/>, so the include dir must
    # come from pkg-config rather than the default search path.
    #
    # GLOBAL: an IMPORTED target is directory-scoped by default, and the unit
    # tests reuse this target's LINK_LIBRARIES (TYPICAL_TEST_LINK_LIBS) from
    # test/. Without GLOBAL, every test target fails to configure with
    # "PkgConfig::DRM ... but the target was not found" whenever the shell links
    # libdrm from here rather than from cmake/drm_kms.cmake (which runs at
    # top-level scope and so is visible everywhere).
    if (NOT TARGET PkgConfig::DRM)
        pkg_check_modules(DRM REQUIRED IMPORTED_TARGET GLOBAL libdrm)
    endif ()
    target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::DRM)
endif ()

if (BUILD_BACKEND_SOFTWARE)
    target_sources(${PROJECT_NAME} PRIVATE
            backend/software/file_sink.cc
            backend/software/sink_factory.cc
            backend/software/software_backend.cc
            backend/software/software_cursor.cc
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
    # Themed software cursor via drm-cxx's drm::cursor loader.
    #   - A DRM backend is on: it already links drm-cxx::drm-cxx, which carries
    #     the same drm::cursor::{Theme,Cursor} objects + headers (drm-cxx/src is
    #     on the include path). Reuse it — linking the standalone loader too
    #     would duplicate those symbols. Just enable the code path.
    #   - Otherwise: the in-tree drm_cxx_cursor_load target (no display deps;
    #     cmake/drm_cxx_cursor_load.cmake), absent when the drm-cxx submodule
    #     isn't checked out — the cursor then stays the embedded arrow.
    if (BUILD_BACKEND_DRM_KMS_EGL OR BUILD_BACKEND_DRM_KMS_VULKAN)
        target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_SW_CURSOR_THEME=1)
    elseif (TARGET drm_cxx_cursor_load)
        target_link_libraries(${PROJECT_NAME} PRIVATE drm_cxx_cursor_load)
        target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_SW_CURSOR_THEME=1)
    endif ()
endif ()

# Shared libinput keyboard core (xkb translation + key-repeat) used by every
# libinput-backed seat — SoftwareSeat and (DRM) DrmSeat — so keyboard behavior
# is identical across backends. Depends only on libxkbcommon, which those seats
# already link. Added once here to avoid duplicate sources in multi-backend
# builds.
if (BUILD_SOFTWARE_INPUT_LIBINPUT OR BUILD_BACKEND_DRM_KMS_EGL OR
    BUILD_BACKEND_DRM_KMS_VULKAN)
    target_sources(${PROJECT_NAME} PRIVATE
            input/xkb_keyboard.cc
            input/key_repeater.cc
            # eventfd waker that breaks the seat's dispatch poll out of an
            # infinite block on a cross-thread flag change (stop / session
            # pause-resume / keymap reload).
            input/wake_event_fd.cc
    )
endif ()

if (BUILD_BACKEND_DRM_KMS_EGL)
    target_sources(${PROJECT_NAME} PRIVATE
            backend/drm_kms_egl/drm_backend.cc
            backend/drm_kms_egl/drm_session.cc
            backend/drm_kms_egl/driver_probe.cc
            backend/drm_kms_egl/gl_cursor.cc
            backend/gl_process_resolver.cc
            display/drm_display.cc
            display/drm_output_provider.cc
            display/drm_device_resolver.cc
            display/drm_mode_list.cc
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
        # LayerScene-driven non-framed present path. scene_layer_source_vk is
        # built dormant: it provides the interface for an Impeller-on-Linux
        # backing-store wrapper that lights up once the Vulkan renderer config
        # is wired through; nothing references it at runtime today.
        target_sources(${PROJECT_NAME} PRIVATE
                backend/drm_kms_egl/scene_layer_source_gl.cc
                backend/drm_kms_egl/scene_layer_source_vk.cc
        )
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

if (BUILD_BACKEND_DRM_KMS_VULKAN)
    # The Vulkan render/scanout sources are the new backend dir; the session /
    # display / seat / driver-probe TUs are shared verbatim with drm_kms_egl
    # (they sit below the pixel layer and are GL-free).
    target_sources(${PROJECT_NAME} PRIVATE
            backend/drm_kms_vulkan/vulkan_drm_backend.cc
            backend/drm_kms_vulkan/device_caps.cc
            backend/drm_kms_vulkan/vulkan_backing_store.cc
            backend/drm_kms_vulkan/drm_scanout_target.cc
            backend/drm_kms_vulkan/modifier_format.cc
            backend/drm_kms_egl/scene_layer_source_vk.cc
            backend/drm_kms_egl/drm_session.cc
            backend/drm_kms_egl/driver_probe.cc
            display/drm_display.cc
            display/drm_output_provider.cc
            display/drm_device_resolver.cc
            display/drm_mode_list.cc
            input/drm_seat.cc
    )
    # HW cursor shares drm_kms_egl's drm-cxx-backed cursor (GL-free). Mirror
    # the EGL backend's libxcursor probe so the optionality matches.
    pkg_check_modules(XCURSOR xcursor)
    if (XCURSOR_FOUND)
        target_sources(${PROJECT_NAME} PRIVATE
                backend/drm_kms_egl/drm_cursor.cc
        )
        target_compile_definitions(${PROJECT_NAME} PRIVATE HAVE_DRM_CURSOR=1)
    else ()
        message(STATUS "libxcursor not found; DRM HW cursor disabled")
    endif ()
    # No EGL/GLESv2: the Vulkan backend produces no GL textures, and
    # flutter_desktop.cc's glDeleteTextures branch is compiled out for it
    # (same as the Wayland-Vulkan backend). Vulkan is resolved at runtime via
    # vulkan.hpp's dynamic loader; only the vendored headers (vulkan_headers)
    # are needed at build time — never a Vulkan SDK in the system/sysroot.
    target_link_libraries(${PROJECT_NAME} PRIVATE
            drm-cxx::drm-cxx
            PkgConfig::DRM
            PkgConfig::GBM
            PkgConfig::LIBINPUT
            PkgConfig::UDEV
            vulkan_headers
    )
endif ()

# Standalone zero-copy-capability probe. Builds with the DRM/KMS Vulkan backend,
# or on its own via BUILD_DRM_KMS_VULKAN_PROBE so it can be cross-built and run
# on a target alongside any backend to report whether the device can do
# zero-copy dma-buf scanout. device_caps.cc has no shell/EGL/Wayland/DRM deps —
# only the Vulkan headers + libdl (vulkan.hpp dlopens libvulkan at runtime).
# Exit code: 0 = gate passed, 1 = refused.
if (BUILD_BACKEND_DRM_KMS_VULKAN OR BUILD_DRM_KMS_VULKAN_PROBE)
    add_executable(drm_kms_vulkan_probe
            backend/drm_kms_vulkan/probe_main.cc
            backend/drm_kms_vulkan/device_caps.cc
            backend/drm_kms_vulkan/modifier_format.cc
    )
    # libdrm: the probe reads the primary plane's IN_FORMATS modifiers read-only
    # to report what layouts the display can scan out. Guarded so the standalone
    # probe build (BUILD_DRM_KMS_VULKAN_PROBE) finds it too.
    if (NOT TARGET PkgConfig::DRM)
        pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
    endif ()
    # Use the vendored Vulkan headers directly so the probe is self-contained
    # and cross-builds without a Vulkan SDK in the target sysroot. Only the
    # headers are needed at build time; libvulkan is dlopened at runtime.
    target_include_directories(drm_kms_vulkan_probe PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include
    )
    target_link_libraries(drm_kms_vulkan_probe PRIVATE ${CMAKE_DL_LIBS} PkgConfig::DRM)
endif ()

# Attach the IHS logging surface (logging.h shim + logger.hpp over ihs_shared).
# Always active; ENABLE_DLT only adds the DLT sink inside ihs_shared.
ihs_target_add_logging(${PROJECT_NAME})

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
    # When the Wayland-Vulkan backend coexists with another Vulkan backend
    # (DRM-KMS-Vulkan) in one binary, several TUs include <vulkan/vulkan.h>
    # (register_backends.cc / flutter_view.cc pull in both backend headers).
    # The first include wins via the header guard, so the Wayland WSI platform
    # must be configured globally: otherwise a TU that reaches the DRM backend
    # header (plain <vulkan/vulkan.h>) first locks vulkan.h WITHOUT
    # VK_KHR_wayland_surface, and wayland_vulkan.h's later vulkan.hpp then
    # references the now-absent VkWaylandSurfaceCreateInfoKHR. Defining the
    # macro on the target keeps every TU's vulkan.h configuration consistent.
    # VkWaylandSurfaceCreateInfoKHR uses opaque struct wl_display* / wl_surface*
    # so this pulls in no wayland-client.h dependency.
    target_compile_definitions(${PROJECT_NAME} PRIVATE
            VK_USE_PLATFORM_WAYLAND_KHR=1)
endif ()
# Dear ImGui debug HUD (frame stats + per-view tracking + live toggles). The
# shared UI core is built once; the Vulkan and/or GL render backend is added per
# the enabled backends, matching third_party/imgui's conditional impl sources.
if (BUILD_HUD AND BUILD_COMPOSITOR AND (BUILD_BACKEND_WAYLAND_VULKAN OR
        BUILD_BACKEND_DRM_KMS_VULKAN OR BUILD_BACKEND_WAYLAND_EGL OR
        BUILD_BACKEND_DRM_KMS_EGL))
    target_sources(${PROJECT_NAME} PRIVATE backend/hud/hud_base.cc)
    if (BUILD_BACKEND_WAYLAND_VULKAN OR BUILD_BACKEND_DRM_KMS_VULKAN)
        target_sources(${PROJECT_NAME} PRIVATE backend/hud/vulkan_hud.cc)
    endif ()
    if (BUILD_BACKEND_WAYLAND_EGL OR BUILD_BACKEND_DRM_KMS_EGL)
        target_sources(${PROJECT_NAME} PRIVATE backend/hud/gl_hud.cc)
    endif ()
    target_link_libraries(${PROJECT_NAME} PRIVATE imgui)
endif ()

if (BUILD_CRASH_HANDLER)
    target_link_libraries(${PROJECT_NAME} PRIVATE sentry::sentry)
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

# Wayland backends: generate the C++ protocol headers (wayland-cxx-scanner) and
# link the wl/ framework + libwayland runtime onto the shell binary. Both helpers
# are defined in cmake/wayland_cxx.cmake. Non-Wayland binaries (DRM EGL/Vulkan,
# software) get neither the headers nor the libwayland-client / -cursor deps.
#
# Gated on IVI_WAYLAND_ANY so wayland-leased-drm is included: it opens a
# wl_display to negotiate a DRM lease. Both helpers self-narrow for that case —
# only core + drm-lease-v1 are generated, and wayland-cursor/xkbcommon are not
# required — so a lease-only build stays free of the surface deps.
if (IVI_WAYLAND_ANY)
    ivi_wayland_protocols(${PROJECT_NAME})
    ivi_wayland_link(${PROJECT_NAME})
endif ()

target_link_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_BINARY_DIR}
)

add_sanitizers(${PROJECT_NAME})

install(TARGETS ${PROJECT_NAME} DESTINATION bin)
