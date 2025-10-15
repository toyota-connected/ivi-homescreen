
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

macro(PLUGIN_LIBRARY plugin_lib_name default)
    # plugin_lib_name"Ivi Homescreen Plugins" -> underscore_fname"Ivi_Homescreen_Plugins"
    string(REPLACE " " "_" underscore_fname ${plugin_lib_name})
    # plugin_lib_name"Ivi Homescreen Plugins" -> dash_fname"Ivi-Homescreen-Plugins"
    string(REPLACE " " "-" dash_fname ${plugin_lib_name})
    # plugin_lib_name"Ivi Homescreen Plugins" -> camelcase_name"IviHomescreenPlugins"
    string(REPLACE " " "" camelcase_name ${plugin_lib_name})
    # underscore_fname"Ivi_Homescreen_Plugins" -> lib"ivi_homescreen_plugins"
    string(TOLOWER ${underscore_fname} lib)
    # dash_fname"Ivi-Homescreen-Plugins" -> dash_lib"ivi-homescreen-plugins"
    string(TOLOWER ${dash_fname} dash_lib)
    # lib"ivi_homescreen_plugins" -> ucase_lib"IVI_HOMESCREEN_PLUGINS"
    string(TOUPPER ${lib} ucase_lib)


    string(APPEND PLUGIN_LIBRARY_LIST "${plugin_lib_name};")
    set(PLUGIN_LIBRARY_LIST "${PLUGIN_LIBRARY_LIST}" PARENT_SCOPE)

    # After including a plugin library (eg. ivi-homescreen-plugins)
    # the `PLUGINS_LIST` variable will contain a semicolon-separated list of
    # camel-case (with spaces) plugin names, e.g. "Url Launcher;Video Player Linux"
    #
    # This populates the PLUGIN_LIB_HEADERS and PLUGIN_LIB_ARRAY variables
    set (plugin_lib_header "${lib}_library.h")
    set (plugin_lib_header_check_path "${CMAKE_CURRENT_SOURCE_DIR}/${plugin_lib_header}")
    # Make sure the header file exists
    if (NOT EXISTS "${plugin_lib_header_check_path}")
        message(FATAL_ERROR "Plugin header file not found: ${plugin_lib_header_check_path}")
    endif ()

    string (APPEND PLUGIN_LIB_HEADERS "#include \"${plugin_lib_header}\"\n")
    string (APPEND PLUGIN_LIB_ARRAY "    ${camelcase_name}PluginLibrary,\n")

    # propagate to parent scope
    set(PLUGIN_LIB_HEADERS "${PLUGIN_LIB_HEADERS}" PARENT_SCOPE)
    set(PLUGIN_LIB_ARRAY "${PLUGIN_LIB_ARRAY}" PARENT_SCOPE)

endmacro(PLUGIN_LIBRARY)

if (ENABLE_PLUGINS)
    if (POLICY CMP0076)
        cmake_policy(SET CMP0076 NEW)
    endif (POLICY CMP0076)

    target_sources(platform_homescreen PUBLIC
            platform_views/platform_views_handler.cc
            platform_views/platform_view_touch.cc
    )

    # Official plugins
    target_include_directories(platform_homescreen PUBLIC ${PLUGINS_DIR})

    # External Plugins
    foreach (EXT_PLUGINS_DIR ${EXT_PLUGINS_DIRS})
        if (EXISTS ${EXT_PLUGINS_DIR})
            target_include_directories(platform_homescreen PUBLIC ${EXT_PLUGINS_DIR})
        else()
            message(STATUS "External plugins directory not found: ${EXT_PLUGINS_DIR}")
        endif ()
    endforeach ()

    target_link_libraries(platform_homescreen PUBLIC plugins)
else ()
    set (PLUGIN_LIB_HEADERS "")
    set (PLUGIN_LIB_ARRAY "")
endif ()
