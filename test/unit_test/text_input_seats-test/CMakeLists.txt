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

# One text input per Wayland seat (#896). Header-only and driven through a fake
# backend, so no compositor is needed; only the wl::ime facade header comes from
# outside.
set(TESTCASE_NAME "homescreen_text_input_seats_ut_test_driver")

add_executable(${TESTCASE_NAME} test_case_text_input_seats.cc)

add_sanitizers(${TESTCASE_NAME})

target_include_directories(${TESTCASE_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/shell
        ${PROJECT_SOURCE_DIR}/third_party/wayland-cxx-scanner/include)
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
