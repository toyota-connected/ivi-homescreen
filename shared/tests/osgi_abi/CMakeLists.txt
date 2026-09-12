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
# ABI conformance for the OSGi handshake structs. Standalone (like
# tests/consumer): configured from an empty tree against the INSTALLED package,
# which means it only builds when that package was installed with
# ENABLE_OSGI=ON -- ihs_osgi.h is not installed otherwise. Driven by
# .github/workflows/ihs-shared.yml.
#
# Most of the work happens at compile time: the assertions are _Static_assert,
# so a field that moved fails the build rather than the run.

cmake_minimum_required(VERSION 3.16)
project(ihs_osgi_abi LANGUAGES C)

find_package(ivi-homescreen-shared CONFIG REQUIRED)

add_executable(ihs_osgi_abi osgi_abi.c)
# Strict C11, for the same reason as tests/consumer: the installed headers must
# stay C-clean, which is what keeps them parseable by ffigen.
set_target_properties(ihs_osgi_abi PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
target_link_libraries(ihs_osgi_abi PRIVATE ivi_homescreen::ihs_shared)

enable_testing()
add_test(NAME ihs_osgi_abi COMMAND ihs_osgi_abi)
