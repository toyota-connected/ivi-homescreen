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
# Coverage-guided fuzz targets. Built only when BUILD_FUZZERS is on, which
# requires clang -- libFuzzer ships with it and has no GCC equivalent.
#
# These are not registered with CTest. A fuzz target runs until stopped, so a
# test suite would either never finish or would run it for a token second and
# report a pass that means nothing. Regression inputs are what belongs in
# CTest, and they go in the unit tests beside the code they pin.

# The instrumentation is applied build-wide by BUILD_FUZZERS (cmake/options.cmake)
# so that ihs_shared is covered too; all that is left here is to pull in
# libFuzzer's main(), which only these targets want.
#
# Deliberately not add_sanitizers(): the sanitizer set is already chosen, and
# a second -fsanitize list on the same link is a toolchain error at best.
function(ihs_add_fuzzer name)
    add_executable(${name} ${ARGN})
    target_link_options(${name} PRIVATE -fsanitize=fuzzer)
    target_link_libraries(${name} PRIVATE ivi_homescreen::ihs_shared)
    set_target_properties(${name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz")
endfunction()

if (BUILD_MCP)
    ihs_add_fuzzer(fuzz_mcp_transport fuzz_mcp_transport.cc)
endif ()
