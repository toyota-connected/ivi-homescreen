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

add_executable(parse_tpv_test parse_tpv_test.cc ${_loc_src}/gpsd_provider.cc)
target_include_directories(parse_tpv_test PRIVATE ${_loc_src})
target_compile_options(parse_tpv_test PRIVATE -Wall -Wextra)
target_link_libraries(parse_tpv_test PRIVATE Threads::Threads)

add_test(NAME parse_tpv COMMAND parse_tpv_test)
