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

# The wl_touch per-scan accumulator: one hardware scan is one batch, under one
# timestamp, for any number of contacts and on a compositor that drops the frame
# after an up.
#
# Compiles the unit on its own rather than linking the shell. That is the point
# of the split: #714's state machine lived in display.cc, which needs a live
# compositor to construct, so nothing reached it and a single-contact assumption
# shipped. Only the embedder header (FlutterPointerPhase) comes from outside.
set(TESTCASE_NAME "homescreen_touch_frame_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_touch_frame.cc
        ${PROJECT_SOURCE_DIR}/shell/wayland/touch_frame.cc
)

add_sanitizers(${TESTCASE_NAME})

target_include_directories(${TESTCASE_NAME} PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main flutter)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
