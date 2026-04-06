#
# Copyright 2026 Toyota Connected North America
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# HomescreenPlugins.cmake
#
# Centralizes plugin discovery, configuration, and (later) signing for
# the homescreen embedder. The user-facing surface is four cache
# variables (declared in cmake/options.cmake) plus two functions:
#
#   homescreen_collect_plugin_dirs()
#       Resolves the in-tree + external plugin dir lists into
#       HOMESCREEN_RESOLVED_STATIC_PLUGIN_DIRS and
#       HOMESCREEN_RESOLVED_DYNAMIC_PLUGIN_DIRS, dedups, and sets
#       ENABLE_PLUGINS for the legacy config_common.h template.
#       Must be called before configure_file() of config_common.h.in.
#
#   homescreen_configure_all_plugins()
#       Walks the resolved lists, includes each plugin's plugin.cmake,
#       and dispatches to add_subdirectory() (static) or a per-plugin
#       add_custom_command() (dynamic). Must be called after the
#       platform_homescreen target exists.
#

include_guard(GLOBAL)

#
# Internal: forward a deprecated cache/var name to its replacement.
#
function(_homescreen_migrate_legacy_var legacy_name new_name)
    if(DEFINED ${legacy_name} AND NOT "${${legacy_name}}" STREQUAL "")
        message(DEPRECATION
            "${legacy_name} is deprecated; use ${new_name} instead. "
            "Forwarding values for this build.")
        set(_merged "${${new_name}}")
        list(APPEND _merged ${${legacy_name}})
        set(${new_name} "${_merged}" PARENT_SCOPE)
    endif()
endfunction()

#
# Resolve in-tree + external plugin dir lists.
#
# Reads:
#   HOMESCREEN_PLUGINS_STATIC, HOMESCREEN_PLUGINS_DYNAMIC,
#   HOMESCREEN_EXTERNAL_PLUGINS_STATIC, HOMESCREEN_EXTERNAL_PLUGINS_DYNAMIC
#
# Writes (PARENT_SCOPE):
#   HOMESCREEN_RESOLVED_STATIC_PLUGIN_DIRS
#   HOMESCREEN_RESOLVED_DYNAMIC_PLUGIN_DIRS
#   ENABLE_PLUGINS  (ON if either list is non-empty)
#
# Conflict resolution: if a directory appears in both static and
# dynamic lists, dynamic wins (matches the integration plan).
#
function(homescreen_collect_plugin_dirs)
    # Apply deprecation shim for the old names. The migration helper
    # writes into our local scope so the rest of this function sees
    # the merged values.
    _homescreen_migrate_legacy_var(STATIC_PLUGIN_DIRS  HOMESCREEN_PLUGINS_STATIC)
    _homescreen_migrate_legacy_var(DYNAMIC_PLUGIN_DIRS HOMESCREEN_PLUGINS_DYNAMIC)

    set(_static  "${HOMESCREEN_PLUGINS_STATIC}")
    set(_dynamic "${HOMESCREEN_PLUGINS_DYNAMIC}")
    list(APPEND _static  ${HOMESCREEN_EXTERNAL_PLUGINS_STATIC})
    list(APPEND _dynamic ${HOMESCREEN_EXTERNAL_PLUGINS_DYNAMIC})

    if(_static)
        list(REMOVE_DUPLICATES _static)
    endif()
    if(_dynamic)
        list(REMOVE_DUPLICATES _dynamic)
        # Dynamic wins on conflict.
        foreach(_d IN LISTS _dynamic)
            list(REMOVE_ITEM _static "${_d}")
        endforeach()
    endif()

    set(HOMESCREEN_RESOLVED_STATIC_PLUGIN_DIRS  "${_static}"  PARENT_SCOPE)
    set(HOMESCREEN_RESOLVED_DYNAMIC_PLUGIN_DIRS "${_dynamic}" PARENT_SCOPE)

    if(_static OR _dynamic)
        set(ENABLE_PLUGINS ON PARENT_SCOPE)
        message(STATUS "Plugins enabled!")
    else()
        message(STATUS "No plugins enabled :c")
    endif()
endfunction()

#
# Internal: clear the well-known variables a plugin.cmake file is
# expected to set, so leftovers from a prior plugin in the same
# configure run cannot leak into the next.
#
macro(_homescreen_clear_plugin_metadata)
    foreach(_v PLUGIN PLUGIN_NAME PLUGIN_FULL_NAME PLUGIN_DESCRIPTION
               PLUGIN_VERSION PLUGIN_TARGET_NAME PLUGIN_HEADER
               PLUGIN_REGISTER_ENDPOINT undescore_fname camelcase_fname
               lcase_name)
        unset(${_v})
    endforeach()
endmacro()

#
# Internal: configure a single plugin directory in the requested mode
# ("static" or "dynamic"). Side effects depend on mode.
#
function(_homescreen_configure_plugin plugin_dir mode)
    if(NOT IS_ABSOLUTE "${plugin_dir}")
        message(FATAL_ERROR
            "homescreen plugin dir must be an absolute path, got '${plugin_dir}'")
    endif()
    if(NOT EXISTS "${plugin_dir}/plugin.cmake")
        message(FATAL_ERROR
            "homescreen plugin dir '${plugin_dir}' is missing plugin.cmake")
    endif()

    _homescreen_clear_plugin_metadata()
    include("${plugin_dir}/plugin.cmake")

    if(NOT DEFINED PLUGIN_NAME)
        message(FATAL_ERROR
            "${plugin_dir}/plugin.cmake did not set PLUGIN_NAME")
    endif()
    if(NOT DEFINED PLUGIN_TARGET_NAME)
        message(FATAL_ERROR
            "${plugin_dir}/plugin.cmake did not set PLUGIN_TARGET_NAME")
    endif()

    message(STATUS "Configuring plugin ... ${PLUGIN}")
    message(STATUS "  name ............... ${PLUGIN_NAME}")
    message(STATUS "  description ........ ${PLUGIN_DESCRIPTION}")
    message(STATUS "  link mode .......... ${mode}")
    message(STATUS "  target ............. ${PLUGIN_TARGET_NAME}")
    message(STATUS "  header ............. ${PLUGIN_HEADER}")
    message(STATUS "  register endpoint .. ${PLUGIN_REGISTER_ENDPOINT}")

    string(TOUPPER "${PLUGIN_NAME}" _upper)
    set(ENABLE_${_upper} ON PARENT_SCOPE)

    if(mode STREQUAL "static")
        add_subdirectory("${plugin_dir}"
            "${PROJECT_BINARY_DIR}/plugins/${PLUGIN_NAME}")
        if(COMMAND add_sanitizers)
            add_sanitizers(${PLUGIN_TARGET_NAME})
        endif()
        if(IPO_SUPPORT_RESULT)
            set_property(TARGET ${PLUGIN_TARGET_NAME}
                PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
        target_link_libraries(plugins PUBLIC ${PLUGIN_TARGET_NAME})

        # Accumulate include + registration lines on global properties
        # so the top-level configure_file() can stitch them together
        # without us having to thread variables through PARENT_SCOPE.
        set_property(GLOBAL APPEND_STRING PROPERTY HOMESCREEN_PLUGIN_INCLUDES
            "#include \"${PLUGIN_HEADER}\"\n")
        if(DEFINED PLUGIN_REGISTER_ENDPOINT)
            set_property(GLOBAL APPEND_STRING
                PROPERTY HOMESCREEN_PLUGIN_REGISTRATIONS
                "  ${PLUGIN_REGISTER_ENDPOINT}(FlutterDesktopGetPluginRegistrar(engine, \"\"));\n")
        else()
            message(WARNING
                "PLUGIN_REGISTER_ENDPOINT not defined for plugin "
                "${PLUGIN_NAME}; skipping registration call.")
        endif()
    elseif(mode STREQUAL "dynamic")
        add_custom_command(
            OUTPUT ${PLUGIN_TARGET_NAME}
            COMMAND ${CMAKE_COMMAND} --target ${PLUGIN_TARGET_NAME}
                    --build ${CMAKE_BINARY_DIR}
                    -DIVI_HOMESCREEN_INCLUDE_DIR=${CMAKE_SOURCE_DIR}/shell/platform/homescreen
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Building plugin ${PLUGIN_NAME} as shared library")
    else()
        message(FATAL_ERROR
            "homescreen plugin mode must be 'static' or 'dynamic', got '${mode}'")
    endif()
endfunction()

#
# Configure all plugins in the resolved lists.
#
# Requires homescreen_collect_plugin_dirs() to have been called first
# and the 'plugins' STATIC library + 'flutter' / 'rapidjson' / 'spdlog'
# imported targets to exist.
#
function(homescreen_configure_all_plugins)
    set(_static  "${HOMESCREEN_RESOLVED_STATIC_PLUGIN_DIRS}")
    set(_dynamic "${HOMESCREEN_RESOLVED_DYNAMIC_PLUGIN_DIRS}")

    if(_static)
        message(STATUS "Static plugins: ${_static}")
        find_package(PkgConfig REQUIRED)

        add_library(plugins STATIC
            ${PROJECT_BINARY_DIR}/generated_plugin_registrant.cc)
        target_include_directories(plugins PUBLIC
            ${CMAKE_SOURCE_DIR}/shell
            ${CMAKE_SOURCE_DIR}/shell/platform/homescreen/public
            ${PROJECT_BINARY_DIR}
            ${CMAKE_BINARY_DIR})
        target_link_libraries(plugins PUBLIC rapidjson spdlog flutter)

        # Reset accumulators in case configure runs more than once.
        set_property(GLOBAL PROPERTY HOMESCREEN_PLUGIN_INCLUDES "")
        set_property(GLOBAL PROPERTY HOMESCREEN_PLUGIN_REGISTRATIONS "")

        foreach(_dir IN LISTS _static)
            _homescreen_configure_plugin("${_dir}" static)
        endforeach()

        get_property(PLUGIN_INCLUDES GLOBAL
            PROPERTY HOMESCREEN_PLUGIN_INCLUDES)
        get_property(PLUGIN_REGISTRATIONS GLOBAL
            PROPERTY HOMESCREEN_PLUGIN_REGISTRATIONS)
        configure_file(
            ${CMAKE_SOURCE_DIR}/shell/generated_plugin_registrant.cc.in
            ${PROJECT_BINARY_DIR}/generated_plugin_registrant.cc
            @ONLY)
    endif()

    if(_dynamic)
        message(STATUS "Dynamic plugins: ${_dynamic}")
        set(BUILD_SHARED_PLUGIN ON PARENT_SCOPE)
        foreach(_dir IN LISTS _dynamic)
            _homescreen_configure_plugin("${_dir}" dynamic)
        endforeach()
    endif()
endfunction()
