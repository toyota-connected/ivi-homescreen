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

# wl_vulkan::LayerCompositor::DrawLayer on a real Vulkan device, read back: a
# layer's source crop, buffer transform and opaque flag land the pixels where
# the geometry says. The Vulkan counterpart of layer_draw_gl-test. Skips
# itself without a Vulkan loader or a device with a graphics queue.
set(TESTCASE_NAME "homescreen_layer_draw_vk_ut_test_driver")

add_executable(${TESTCASE_NAME} test_case_layer_draw_vk.cc)
add_sanitizers(${TESTCASE_NAME})
target_include_directories(${TESTCASE_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include)
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
