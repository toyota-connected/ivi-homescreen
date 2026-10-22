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

# xdg_toplevel.close ends the shell, against a mock compositor (no GPU, no
# compositor). Links homescreen_ut_shell for XdgShell, WaylandWindow's close
# handler and the run flag.
set(TESTCASE_NAME "homescreen_toplevel_close_ut_test_driver")

find_package(PkgConfig REQUIRED)
pkg_check_modules(WL_SERVER REQUIRED IMPORTED_TARGET wayland-server)

add_executable(${TESTCASE_NAME} test_case_toplevel_close.cc)
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(
        ${TESTCASE_NAME}
        PRIVATE
        gtest_main
        homescreen_ut_shell
        PkgConfig::WL_SERVER
        Threads::Threads
)
if (BUILD_BACKEND_WAYLAND_EGL)
    target_link_libraries(${TESTCASE_NAME} PRIVATE PkgConfig::EGL)
endif ()

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
