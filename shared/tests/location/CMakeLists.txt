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
# Unit test for the internal gpsd TPV parser. Standalone (like tests/consumer):
# configured from an empty tree; compiles the provider source directly because
# ParseTpv is internal, not part of the installed C ABI. Driven by
# .github/workflows/ihs-shared.yml.

cmake_minimum_required(VERSION 3.16)
project(ihs_location_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

enable_testing()
find_package(Threads REQUIRED)

set(_loc_src ${CMAKE_CURRENT_SOURCE_DIR}/../../src/location)
set(_loc_inc ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

add_executable(parse_tpv_test parse_tpv_test.cc ${_loc_src}/gpsd_provider.cc)
target_include_directories(parse_tpv_test PRIVATE ${_loc_src})
target_compile_options(parse_tpv_test PRIVATE -Wall -Wextra)
target_link_libraries(parse_tpv_test PRIVATE Threads::Threads)

add_test(NAME parse_tpv COMMAND parse_tpv_test)

# Measurement source / filter registry. Compiles registry.cc directly (the
# Lookup* API is internal) and reaches the public header for IhsMeasurement etc.
# That header includes the CMake-generated ihs/ihs_export.h (IHS_EXPORT), which
# only the full library build produces. This test links everything statically,
# so a trivial empty IHS_EXPORT stands in — no installed library needed.
set(_export_stub ${CMAKE_CURRENT_BINARY_DIR}/gen/ihs/ihs_export.h)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gen/ihs)
file(WRITE ${_export_stub}
        "#ifndef IHS_EXPORT_H\n#define IHS_EXPORT_H\n"
        "#define IHS_EXPORT\n#define IHS_NO_EXPORT\n#endif\n")

add_executable(registry_test registry_test.cc ${_loc_src}/registry.cc)
target_include_directories(registry_test PRIVATE
        ${_loc_src} ${_loc_inc} ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_compile_options(registry_test PRIVATE -Wall -Wextra)
target_link_libraries(registry_test PRIVATE Threads::Threads)

add_test(NAME registry COMMAND registry_test)
