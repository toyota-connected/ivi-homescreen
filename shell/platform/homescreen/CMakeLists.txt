
add_library(platform_homescreen STATIC
        flutter_desktop.cc
        flutter_desktop_messenger.cc
        platform_handler.cc
        platform_views/platform_views_handler.cc
        platform_views/platform_view_touch.cc
)

target_include_directories(platform_homescreen PUBLIC
        .
        ../../../plugins/layer_playground
        client_wrapper/include
        public
        ${CMAKE_BINARY_DIR}/shell
        ${CMAKE_SOURCE_DIR}/shell
)

target_include_directories(platform_homescreen PRIVATE
        ../..
        ../../..
        ../../logging
        ../../../plugins
        platform_views
)

target_compile_definitions(platform_homescreen PUBLIC
        PATH_PREFIX="${CMAKE_INSTALL_PREFIX}"
)

target_link_libraries(platform_homescreen PUBLIC
        asio
        spdlog
        wayland-gen
        flutter_common
)

if (BUILD_PLUGIN_LAYER_PLAYGROUND_VIEW)
    target_link_libraries(platform_homescreen PUBLIC plugin_layer_playground_view)
endif ()

if (BUILD_PLUGIN_FILAMENT_VIEW)
    target_link_libraries(platform_homescreen PUBLIC plugin_filament_view)
endif ()

