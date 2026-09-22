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

# EglDmabufImporter::ImportableModifiers against a real EGL display (Mesa's
# surfaceless platform, so no window system or DRM master). Skips itself where
# the platform or EGL_EXT_image_dma_buf_import_modifiers is missing.
set(TESTCASE_NAME "homescreen_egl_import_modifiers_ut_test_driver")

add_executable(${TESTCASE_NAME} test_case_egl_import_modifiers.cc)
add_sanitizers(${TESTCASE_NAME})
target_include_directories(${TESTCASE_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/shell/platform/homescreen/platform_views)
target_link_libraries(${TESTCASE_NAME} PRIVATE
        gtest_main homescreen_ut_shell EGL)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
