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

# GlCompositor::CompositeLayerToFbo drawn on a real GLES context (Mesa's
# surfaceless EGL platform) and read back: a layer's source crop, buffer
# transform and opaque flag land the pixels where the geometry says. Skips
# itself without the platform or a GLES2 context.
set(TESTCASE_NAME "homescreen_layer_draw_gl_ut_test_driver")

add_executable(${TESTCASE_NAME} test_case_layer_draw_gl.cc)
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME} PRIVATE
        gtest_main homescreen_ut_shell EGL GLESv2)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
