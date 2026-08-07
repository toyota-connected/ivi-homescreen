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
# Covers the shell-side publisher: transform composition to screen space, the
# root-first tree walk, and the mapping between the framework's semantics
# vocabulary and the hub ABI's. Links libihs_shared so PublishTree can be
# driven end to end, but needs no engine.
set(TESTCASE_NAME "homescreen_semantics_publisher_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_semantics_publisher.cc
)

add_sanitizers(${TESTCASE_NAME})

target_link_libraries(${TESTCASE_NAME}
        PRIVATE
        gtest_main
        homescreen_ut_shell
        ivi_homescreen::ihs_shared
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
