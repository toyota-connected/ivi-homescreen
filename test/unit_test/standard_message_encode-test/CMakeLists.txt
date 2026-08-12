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
# Round-trips ihs_shared's encoder against Flutter's own vendored serializer.
# ihs_shared cannot link libflutter -- that is the whole reason the encoder
# exists -- but the test can, which is what makes a second implementation of
# the format safe to keep: a divergence fails here rather than reaching the
# framework as a silently dropped argument.
set(TESTCASE_NAME "homescreen_standard_message_encode_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_standard_message_encode.cc
        # Compiled in rather than linked: these are internal to ihs_shared and
        # deliberately not exported through the ihs_* ABI.
        ${CMAKE_SOURCE_DIR}/shared/src/standard_message_encode.cc
)

add_sanitizers(${TESTCASE_NAME})

target_include_directories(${TESTCASE_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/shared/src
)

target_link_libraries(${TESTCASE_NAME}
        PRIVATE
        gtest_main
        flutter
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
