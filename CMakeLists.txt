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

include(context)
include(options)

message(STATUS "Project ................ ${PROJECT_NAME}")
message(STATUS "Version ................ ${PROJECT_VERSION}")
message(STATUS "Generator .............. ${CMAKE_GENERATOR}")
message(STATUS "Build Type ............. ${CMAKE_BUILD_TYPE}")

include(compiler)
include(utils)

configure_file(cmake/config_common.h.in ${PROJECT_BINARY_DIR}/config/common.h)

#
# libraries
#
add_subdirectory(third_party)
if (ENABLE_PLUGINS)
    add_subdirectory(plugins)
endif ()

add_subdirectory(shell/platform/homescreen)

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
