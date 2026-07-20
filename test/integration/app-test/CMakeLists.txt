# Integration test — NOT a unit test.
#
# This test constructs App(configs) and calls App::Loop(), which internally
# calls App::BuildDisplays() → wl_display_connect() for each Wayland view.
# A running Wayland compositor must be present on WAYLAND_DISPLAY.
#
# To run manually with an active compositor:
#   ninja homescreen_app_ut_test_driver
#   WAYLAND_DISPLAY=wayland-0 ./test/integration/app-test/homescreen_app_ut_test_driver
#
# Do NOT add this to the unit-test-coverage CI job.

set(TESTCASE_NAME "homescreen_app_ut_test_driver")
set(TESTCASE_CC test_case_app.cc)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
