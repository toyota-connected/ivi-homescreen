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
# Standalone out-of-tree consumer of the INSTALLED ihs_shared package. Not part
# of the homescreen build; configured from an empty tree with
# -DCMAKE_PREFIX_PATH=<install prefix> to prove find_package + the C ABI work
# for an external Dart FFI plugin. Driven by .github/workflows/ihs-shared.yml.

cmake_minimum_required(VERSION 3.16)
project(ihs_shared_consumer LANGUAGES C)

find_package(ivi-homescreen-shared CONFIG REQUIRED)

add_executable(ihs_consumer consumer.c)
# Strict C11 so the installed headers are proven C-clean (ffigen-parseable).
set_target_properties(ihs_consumer PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
target_link_libraries(ihs_consumer PRIVATE ivi_homescreen::ihs_shared)

enable_testing()
add_test(NAME ihs_consumer COMMAND ihs_consumer)
