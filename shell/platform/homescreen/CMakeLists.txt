
add_library(platform_homescreen STATIC
        ${PROJECT_BINARY_DIR}/config.h
        flutter_desktop.cc
        flutter_desktop_messenger.cc
        key_event_handler.cc
        logging_handler.cc
        mouse_cursor_handler.cc
        platform_handler.cc
        text_input_plugin.cc
        platform_views/platform_views_handler.cc
        platform_views/platform_view_touch.cc
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
        ../../../plugins
        platform_views
)

target_link_libraries(platform_homescreen PUBLIC
        asio
        flutter
        rapidjson
        spdlog
        wayland-gen
)

if (NOT DISABLE_PLUGINS)
    target_link_libraries(platform_homescreen PUBLIC plugins)
endif ()
