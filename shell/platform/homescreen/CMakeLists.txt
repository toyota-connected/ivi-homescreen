
add_library(platform_homescreen SHARED
        flutter_desktop.cc
        flutter_desktop_messenger.cc
        platform_handler.cc
)

target_include_directories(platform_homescreen PUBLIC
        .
        client_wrapper/include
        public
        ${CMAKE_BINARY_DIR}/shell
)

target_include_directories(platform_homescreen PRIVATE
        ../..
        ../../..
        ../../logging
        ../../../plugins
)

target_compile_definitions(platform_homescreen PUBLIC
        PATH_PREFIX="${CMAKE_INSTALL_PREFIX}"
)

target_link_libraries(platform_homescreen PUBLIC
        asio
        flutter_common
        spdlog
        wayland-gen
)
