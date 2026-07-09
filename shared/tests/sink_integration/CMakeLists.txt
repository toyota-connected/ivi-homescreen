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
# End-to-end logging sink test: drives the installed C ABI with each
# IHS_LOG_SINK / IHS_LOG_LEVEL / IHS_LOG_FILE configuration and checks the
# resulting output. Built out-of-tree against the installed package, like the
# consumer smoke; each scenario is a separate process (ihs_log_start is a
# singleton). Driven by .github/workflows/ihs-shared.yml.

cmake_minimum_required(VERSION 3.16)
project(ihs_sink_integration LANGUAGES C)

find_package(ivi-homescreen-shared CONFIG REQUIRED)

add_executable(sink_integration sink_integration.c)
set_target_properties(sink_integration PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS ON)  # mkstemp/setenv need _DEFAULT_SOURCE
target_link_libraries(sink_integration PRIVATE ivi_homescreen::ihs_shared)

enable_testing()
foreach (scenario console file level fallback)
    add_test(NAME sink_${scenario} COMMAND sink_integration ${scenario})
endforeach ()
