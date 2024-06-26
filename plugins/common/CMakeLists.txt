
add_library(plugin_common STATIC
        json/json_utils.cc
        time/time_tools.cc
        string/string_tools.cc
        tools/encodable.cc
        tools/command.cc
)
target_include_directories(plugin_common PUBLIC . ${PROJECT_BINARY_DIR})
target_compile_definitions(plugin_common PUBLIC EGL_NO_X11)
target_link_libraries(plugin_common PUBLIC flutter)
add_sanitizers(plugin_common)
if (IPO_SUPPORT_RESULT)
    set_property(TARGET plugin_common PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

pkg_check_modules(CURL IMPORTED_TARGET libcurl)
if (CURL_FOUND)
    add_library(plugin_common_curl STATIC curl_client/curl_client.cc)
    target_include_directories(plugin_common_curl PUBLIC . ${PROJECT_BINARY_DIR})
    target_link_libraries(plugin_common_curl PUBLIC PkgConfig::CURL spdlog)
    add_sanitizers(plugin_common_curl)
    if (IPO_SUPPORT_RESULT)
        set_property(TARGET plugin_common_curl PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif ()
endif ()

pkg_check_modules(GLIB IMPORTED_TARGET glib-2.0)
if (GLIB_FOUND)
    add_library(plugin_common_glib STATIC glib/main_loop.cc)
    target_include_directories(plugin_common_glib PUBLIC . ${PROJECT_BINARY_DIR})
    target_link_libraries(plugin_common_glib PUBLIC PkgConfig::GLIB)
    add_sanitizers(plugin_common_glib)
    if (IPO_SUPPORT_RESULT)
        set_property(TARGET plugin_common_glib PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif ()
endif ()