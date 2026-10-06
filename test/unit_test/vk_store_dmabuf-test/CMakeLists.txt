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

# VulkanBackingStore's dma-buf export on a real Vulkan device: the export is
# only planned for a configuration the driver reports exportable, an exported
# store describes its own layout, and creating one emits no validation message.
# Skips itself without a Vulkan loader or an exporting device; the
# validation-clean case also needs VK_LAYER_KHRONOS_validation installed.
set(TESTCASE_NAME "homescreen_vk_store_dmabuf_ut_test_driver")

add_executable(${TESTCASE_NAME} test_case_vk_store_dmabuf.cc)
add_sanitizers(${TESTCASE_NAME})
target_include_directories(${TESTCASE_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include)
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
