#
# Copyright 2020 Toyota Connected North America
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

cmake_minimum_required(VERSION 3.10.2)

if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build, options are: Debug, Release, RelWithDebInfo, or MinSizeRel." FORCE)
    message(STATUS "CMAKE_BUILD_TYPE not set, defaulting to Release.")
endif ()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/third_party/sanitizers-cmake/cmake)

if (NOT BUILD_NUMBER)
    set(BUILD_NUMBER 0)
endif ()

project(ivi-homescreen
        VERSION "1.0.${BUILD_NUMBER}"
        DESCRIPTION "IVI Flutter Shell"
        LANGUAGES CXX C
)

include(git)
include(options)

message(STATUS "Project ................ ${PROJECT_NAME}")
message(STATUS "Version ................ ${PROJECT_VERSION}")
message(STATUS "Generator .............. ${CMAKE_GENERATOR}")
message(STATUS "Build Type ............. ${CMAKE_BUILD_TYPE}")

include(compiler)

configure_file(cmake/config_common.h.in ${PROJECT_BINARY_DIR}/config/common.h)

#
# libraries
#
add_subdirectory(third_party)

add_subdirectory(shell/platform/homescreen)

#
# plugins
#

if (STATIC_PLUGIN_DIRS)
    message (STATUS "Plugins enabled!")
    set(ENABLE_PLUGINS ON)
endif()

if (STATIC_PLUGIN_DIRS)
    set(PLUGIN_INCLUDES "")
    set(PLUGIN_REGISTRATIONS "")

    add_library(plugins STATIC ${PROJECT_BINARY_DIR}/generated_plugin_registrant.cc)
    target_include_directories(plugins PUBLIC ${CMAKE_SOURCE_DIR}/shell ${PROJECT_BINARY_DIR} ${CMAKE_BINARY_DIR})
    target_link_libraries(plugins PUBLIC rapidjson spdlog flutter)

    foreach(plugin_dir IN LISTS STATIC_PLUGIN_DIRS)
        get_filename_component(plugin_name ${plugin_dir} NAME)
        # Read the plugin's metadata
        include(${plugin_dir}/plugin.cmake)
        # print the plugin's metadata for debugging
        message(STATUS "Configuring plugin ... ${PLUGIN}")
        message(STATUS "  name ............... ${PLUGIN_NAME}")
        #message(STATUS "  version ............ ${PLUGIN_VERSION}")
        message(STATUS "  description ........ ${PLUGIN_DESCRIPTION}")
        message(STATUS "  header ............. ${PLUGIN_HEADER}")
        message(STATUS "  register endpoint .. ${PLUGIN_REGISTER_ENDPOINT}")

        # Set enable flags
        set(ENABLE_${PLUGIN_NAME_CAMEL_CASE} ON)

        # Build the plugin
        add_subdirectory(${plugin_dir} ${PROJECT_BINARY_DIR}/plugins/${plugin_name})
        add_sanitizers(plugin_${plugin_name})
        if (IPO_SUPPORT_RESULT)
            set_property(TARGET plugin_${plugin_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
        target_link_libraries(plugins PUBLIC plugin_${plugin_name})

        # Accumulate for registrant
        string(APPEND PLUGIN_INCLUDES "#include \"${PLUGIN_HEADER}\"\n")

        # (check if PLUGIN_REGISTER_ENDPOINT is defined to avoid appending empty registration calls for plugins that don't specify an endpoint)
        if (DEFINED PLUGIN_REGISTER_ENDPOINT)
            string(APPEND PLUGIN_REGISTRATIONS
                "  ${PLUGIN_REGISTER_ENDPOINT}(FlutterDesktopGetPluginRegistrar(engine, \"\"));\n")
        else()
            message(WARNING "PLUGIN_REGISTER_ENDPOINT not defined for plugin ${PLUGIN_NAME}, skipping registration call in generated_plugin_registrant.cc")
        endif()
    endforeach()

    # Clear variables set by plugin.cmake
    unset(PLUGIN_NAME)
    unset(PLUGIN_FULL_NAME)
    unset(PLUGIN_DESCRIPTION)
    unset(PLUGIN_VERSION)
    unset(undescore_fname)
    unset(camelcase_fname)
    unset(lcase_name)
    unset(PLUGIN_HEADER)
    unset(PLUGIN_REGISTER_ENDPOINT)

    configure_file(
        ${CMAKE_SOURCE_DIR}/shell/generated_plugin_registrant.cc.in
        ${PROJECT_BINARY_DIR}/generated_plugin_registrant.cc
        @ONLY
    )
endif()

#
# executable
#
add_subdirectory(shell)

#print_target_properties(asio)
#print_target_properties(flutter)
#print_target_properties(plugin_url_launcher)
#print_target_properties(platform_homescreen)
#print_target_properties(homescreen)


add_subdirectory(docs)

#
# Unit Tests
#
if (BUILD_UNIT_TESTS)
    if (NOT DEFINED UNIT_TEST_APP_BUNDLE)
        set(UNIT_TEST_APP_BUNDLE "/home/root")
    endif ()
    if (UNIT_TEST_SAVE_GOLDENS)
        add_compile_definitions(SAVE_IMAGE_FOR_COMPARISON)
    endif ()
    enable_testing()
    add_subdirectory(test)
    install(DIRECTORY DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/test/unit_test/test_images_golden)
    install(DIRECTORY DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/test/unit_test/test_images)
endif ()

if (NOT CMAKE_CROSSCOMPILING)
    include(packaging)
endif ()
