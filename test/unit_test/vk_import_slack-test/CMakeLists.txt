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

# The arithmetic behind the v3dv short-allocation import (#723): what a driver
# that pads an import requires of the producer's dma-buf, and when asking short
# can succeed. Header-only logic: no Vulkan device, loader or driver needed.
set(TESTCASE_NAME "homescreen_vk_import_slack_ut_test_driver")

add_executable(${TESTCASE_NAME} test_case_vk_import_slack.cc)
add_sanitizers(${TESTCASE_NAME})
target_include_directories(${TESTCASE_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/shell/platform/homescreen/platform_views
        ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include)
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
