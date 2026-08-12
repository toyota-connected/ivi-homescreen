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
# Action-argument encoding: the hub's plain layouts turned into the
# StandardMessageCodec messages the framework decodes. Every case is decoded
# again with the same vendored codec, so a layout mistake fails here rather
# than reaching the framework as an argument dropped in silence (R-9).
set(TESTCASE_NAME "homescreen_semantics_action_args_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_semantics_action_args.cc
        ${CMAKE_SOURCE_DIR}/shell/accessibility/semantics_action_args.cc
)

add_sanitizers(${TESTCASE_NAME})

target_include_directories(${TESTCASE_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/shell
)

target_link_libraries(${TESTCASE_NAME}
        PRIVATE
        gtest_main
        flutter
        ivi_homescreen::ihs_shared
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
