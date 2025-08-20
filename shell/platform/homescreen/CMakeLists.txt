
add_library(platform_homescreen STATIC
        flutter_desktop.cc
        flutter_desktop_messenger.cc
        key_event_handler.cc
        logging_handler.cc
        mouse_cursor_handler.cc
        platform_handler.cc
        text_input_plugin.cc
)

target_include_directories(platform_homescreen PUBLIC
        .
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/shell
        client_wrapper/include
        public
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
        spdlog
        wayland-gen
        tomlplusplus::tomlplusplus
)

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
            platform_views/platform_view_touch.cc
    )

    target_include_directories(platform_homescreen PUBLIC
            ${PLUGINS_DIR}
    )

    target_link_libraries(platform_homescreen PUBLIC
            plugins
    )

endif ()
