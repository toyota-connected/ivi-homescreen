
pkg_check_modules(CURL IMPORTED_TARGET REQUIRED libcurl)
pkg_check_modules(GLIB IMPORTED_TARGET REQUIRED glib-2.0)

add_library(plugin_common STATIC
        curl_client/curl_client.cc
        json/json_utils.cc
        glib/main_loop.cc
        time/time_tools.cc
        string/string_tools.cc
        tools/encodable.cc
        tools/command.cc
)

target_include_directories(plugin_common PRIVATE .)

target_link_libraries(plugin_common PUBLIC
        flutter
        PkgConfig::CURL
        PkgConfig::GLIB
)

add_sanitizers(plugin_common)

if (IPO_SUPPORT_RESULT)
    set_property(TARGET plugin_common PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()
