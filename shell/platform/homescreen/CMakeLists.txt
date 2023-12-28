
add_library(platform_homescreen STATIC
        flutter_desktop.cc
        flutter_desktop_messenger.cc
        key_event_handler.cc
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

target_compile_definitions(platform_homescreen PUBLIC
        PATH_PREFIX="${CMAKE_INSTALL_PREFIX}"
)

target_link_libraries(platform_homescreen PUBLIC
        asio
        spdlog
        wayland-gen
        flutter
)

if (BUILD_PLUGIN_LAYER_PLAYGROUND_VIEW)
    target_link_libraries(platform_homescreen PUBLIC plugin_layer_playground_view)
endif ()

if (BUILD_PLUGIN_FILAMENT_VIEW)
    target_link_libraries(platform_homescreen PUBLIC plugin_filament_view)
endif ()

if (BUILD_AUDIOPLAYERS_LINUX_PLUGIN)
    target_link_libraries(platform_homescreen PUBLIC audioplayers_linux_plugin)
endif ()

if (BUILD_PLUGIN_CLOUD_FIRESTORE)
    target_link_libraries(platform_homescreen PUBLIC plugin_cloud_firestore)
endif ()
if (BUILD_PLUGIN_FIREBASE_AUTH)
    target_link_libraries(platform_homescreen PUBLIC plugin_firebase_auth)
endif ()
if (BUILD_PLUGIN_FIREBASE_STORAGE)
    target_link_libraries(platform_homescreen PUBLIC plugin_firebase_storage)
endif ()
if (BUILD_PLUGIN_FIREBASE_CORE)
    target_link_libraries(platform_homescreen PUBLIC plugin_firebase_core)
endif ()

if (BUILD_PLUGIN_URL_LAUNCHER)
    target_link_libraries(platform_homescreen PUBLIC plugin_url_launcher)
endif ()

if (BUILD_PLUGIN_DESKTOP_WINDOW_LINUX)
    target_link_libraries(platform_homescreen PUBLIC plugin_desktop_window_linux)
endif ()