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
# MCP transport tests: HTTP framing, JSON-RPC dispatch and the socket
# permissions. Speaks to a real socket over loopback-free AF_UNIX, since the
# framing and the file mode are the parts worth proving.
set(TESTCASE_NAME "homescreen_mcp_transport_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_mcp_transport.cc
)

add_sanitizers(${TESTCASE_NAME})

target_link_libraries(${TESTCASE_NAME}
        PRIVATE
        gtest_main
        ivi_homescreen::ihs_shared
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
