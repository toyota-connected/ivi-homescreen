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

# --drm-rotation on drm-kms-egl without the compositor (#848): the frame the
# engine rendered offscreen, turned onto the panel at present. Reads the CRTC
# back, so it needs drm-cxx's capture module; skips itself without vkms.
set(TESTCASE_NAME "homescreen_drm_backend_vkms_turn_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_drm_backend_vkms_turn.cc
)

add_sanitizers(${TESTCASE_NAME})

target_link_libraries(${TESTCASE_NAME}
        PRIVATE
        gtest
        homescreen_ut_shell
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
