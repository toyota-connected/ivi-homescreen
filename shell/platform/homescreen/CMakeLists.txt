
add_library(platform_homescreen STATIC
        flutter_desktop.cc
        flutter_desktop_messenger.cc
        platform_handler.cc
        platform_views/platform_views_handler.cc
        platform_views/platform_views_registry.cc
        platform_views/platform_view_touch.cc
)

target_include_directories(platform_homescreen PUBLIC
        .
        ./platform_views/layer_playground
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
        plugin_filament_view
)

target_compile_definitions(platform_homescreen PUBLIC ENABLE_PLUGIN_FILAMENT_VIEW)

if (BUILD_VIEW_LAYER_PLAYGROUND)
    target_compile_definitions(platform_homescreen PUBLIC ENABLE_PLUGIN_LAYER_PLAYGROUND)
    target_sources(platform_homescreen PRIVATE
            platform_views/layer_playground/layer_playground.cc
    )
endif ()
