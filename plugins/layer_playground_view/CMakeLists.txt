include_guard()

add_library(plugin_layer_playground_view STATIC
        layer_playground_view_plugin.cc
        layer_playground_view_plugin_c_api.cc
)

target_compile_definitions(plugin_layer_playground_view PUBLIC
        MESA_EGL_NO_X11_HEADERS
        WL_EGL_PLATFORM
        EGL_NO_X11
)

target_include_directories(plugin_layer_playground_view PUBLIC
        .
        include
)

target_link_libraries(plugin_layer_playground_view PUBLIC
        flutter
        platform_homescreen
        GLESv2
        EGL
)
