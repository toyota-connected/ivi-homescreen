
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

if (ENABLE_PLUGINS)

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
            platform_views/dmabuf_vulkan_import.cc
    )

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
