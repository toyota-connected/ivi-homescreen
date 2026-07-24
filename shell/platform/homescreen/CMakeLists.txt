
add_library(platform_homescreen STATIC
        flutter_desktop.cc
        flutter_desktop_messenger.cc
        flutter_desktop_texture_registrar.cc
        key_event_handler.cc
        key_mapping.cc
        logging_handler.cc
        mouse_cursor_handler.cc
        platform_handler.cc
        text_input_plugin.cc
        watchdog_plugin.cc
)

# Keep every symbol of this static library out of the executable's dynamic
# symbol table. It implements the Flutter desktop C API (FlutterDesktop*), which
# libihs_shared.so also defines -- as forwarders that re-export the surface for
# out-of-tree plugins. Without hidden visibility the two colliding definitions
# force the linker to export the executable's copies too; being first in symbol
# search order the executable would then preempt the forwarders, so a plugin
# would bind straight to these internal copies and libihs_shared.so's re-export
# would be dead. Hidden here, these stay usable within the executable (the shell
# installs their addresses into the forwarder proc table, see
# shell/libflutter_engine.cc) while libihs_shared.so remains the sole exporter.
set_target_properties(platform_homescreen PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(platform_homescreen PUBLIC
        .
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/shell
        client_wrapper/include
        public
        # config/common.h is generated at ${PROJECT_BINARY_DIR}/config/common.h
        # and pulled in via logging.h / backend.h, so add the binary root and the
        # shell binary dir directly.
        ${CMAKE_BINARY_DIR}
        ${CMAKE_BINARY_DIR}/shell
)

target_include_directories(platform_homescreen PRIVATE
        ../..
        ../../..
        ../../logging
        platform_views
)

target_link_libraries(platform_homescreen PUBLIC
        asio
        flutter
        rapidjson
        fmt
        tomlplusplus::tomlplusplus
)

# key_event_handler.cc / text_input_plugin.cc always use libxkbcommon (xkb_*
# symbols), independent of the input seat or backend. The Wayland / DRM /
# software-input paths each happen to pull it in, but a build with none of
# those (e.g. software backend with BUILD_SOFTWARE_INPUT_LIBINPUT=OFF) would
# leave these symbols undefined — so link it directly here.
find_package(PkgConfig REQUIRED)
pkg_check_modules(XKBCOMMON_PLATFORM REQUIRED IMPORTED_TARGET xkbcommon)
target_link_libraries(platform_homescreen PUBLIC PkgConfig::XKBCOMMON_PLATFORM)

# platform_homescreen needs no Wayland binding dependency: it includes no
# Wayland protocol headers directly. The generated protocol headers attach to
# the shell binary in shell/CMakeLists.txt.

find_package(ACCESSKIT QUIET)
if (ACCESSKIT_FOUND)
    target_include_directories(platform_homescreen PUBLIC
            ${ACCESSKIT_INCLUDE_DIRS}
    )
    target_link_libraries(platform_homescreen PUBLIC accesskit)
    target_compile_definitions(platform_homescreen PUBLIC
            ENABLE_ACCESSKIT
    )
endif ()

# Platform views compose through the compositor (ICompositorSurface), so the
# subsystem is gated on BUILD_COMPOSITOR — not the external plugins framework.
# This keeps platform views (and the ihs_shared ihs_pv host) a core shell
# feature that builds without the ivi-homescreen-plugins repo.
if (BUILD_COMPOSITOR)

    if (POLICY CMP0076)
        cmake_policy(SET CMP0076 NEW)
    endif (POLICY CMP0076)

    target_sources(platform_homescreen PUBLIC
            platform_views/platform_views_handler.cc
            platform_views/platform_view_registry.cc
            platform_views/platform_view_touch.cc
    )

    # The ihs_shared platform-view host reaches the Backend + compositor, so it
    # is shell-only (PRIVATE) — it must not compile into the plugin targets that
    # consume the PUBLIC registry/handler sources above.
    target_sources(platform_homescreen PRIVATE
            platform_views/platform_view_host.cc
    )

    # The Vulkan dma-buf importer is only reachable on a Vulkan backend; it (and
    # the host's Vulkan path) #include <vulkan/vulkan.h>. Compile it and put the
    # vendored Vulkan headers on the path only for Vulkan builds, so an
    # EGL/DRM-EGL/software build without system Vulkan headers stays
    # Vulkan-header-free. Guarded in the host by IVI_HAVE_VULKAN.
    if (BUILD_BACKEND_WAYLAND_VULKAN OR BUILD_BACKEND_DRM_KMS_VULKAN)
        target_sources(platform_homescreen PRIVATE
                platform_views/dmabuf_vulkan_import.cc
        )
        target_include_directories(platform_homescreen PRIVATE
                ${CMAKE_SOURCE_DIR}/third_party/Vulkan-Headers/include)
    endif ()

    # The EGL/GLES dma-buf importer (the GL counterpart of the Vulkan one) is
    # only reachable on an EGL backend; it needs the EGL/GLES + libdrm headers
    # and links EGL/GLESv2. Guarded in the host by IVI_HAVE_EGL.
    if (BUILD_BACKEND_WAYLAND_EGL OR BUILD_BACKEND_DRM_KMS_EGL)
        target_sources(platform_homescreen PRIVATE
                platform_views/egl_dmabuf_import.cc
        )
        if (NOT TARGET PkgConfig::DRM)
            pkg_check_modules(DRM REQUIRED IMPORTED_TARGET GLOBAL libdrm)
        endif ()
        target_link_libraries(platform_homescreen PRIVATE
                EGL GLESv2 PkgConfig::DRM)
    endif ()

endif ()

# The external plugins framework (out-of-tree plugins built from the plugins
# repo) is independent of the platform-view subsystem above.
if (ENABLE_PLUGINS)

    target_include_directories(platform_homescreen PUBLIC
            ${PLUGINS_DIR}
    )

    target_link_libraries(platform_homescreen PUBLIC
            plugins
    )

endif ()

# The Flutter/shell logging surface (logging.h shim over ihs_shared) is used
# throughout this library, so it needs the ihs/* headers and fmt on its path.
ihs_target_add_logging(platform_homescreen)
