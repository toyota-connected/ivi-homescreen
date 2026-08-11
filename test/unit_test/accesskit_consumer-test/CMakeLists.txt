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
# Covers the two translation tables between the semantics hub's vocabulary and
# AccessKit's. Only buildable when the bindings are present, so it is added
# under BUILD_ACCESSKIT alongside the source it tests.
set(TESTCASE_NAME "homescreen_accesskit_consumer_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_accesskit_consumer.cc
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
