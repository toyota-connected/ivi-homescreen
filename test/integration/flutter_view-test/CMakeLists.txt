# Integration test — NOT a unit test.
#
# This test creates a live Wayland compositor connection via Display() and
# constructs a FlutterView backed by it.  A running Wayland compositor
# (e.g. weston, sway) must be present on WAYLAND_DISPLAY for the test to
# pass.  It cannot run in a headless CI unit-test job.
#
# To run manually with an active compositor:
#   ninja homescreen_flutter_view_ut_test_driver
#   WAYLAND_DISPLAY=wayland-0 ./test/integration/flutter_view-test/homescreen_flutter_view_ut_test_driver
#
# Do NOT add this to the unit-test-coverage CI job.

set(TESTCASE_NAME "homescreen_flutter_view_ut_test_driver")
set(TESTCASE_CC test_case_flutter_view.cc)

# Basically, the following statements need not be modified
add_executable(
        ${TESTCASE_NAME}
        ${TYPICAL_TEST_SOURCES}
        ${TESTCASE_CC}
)

add_sanitizers(${TESTCASE_NAME})

target_compile_definitions(
        ${TESTCASE_NAME}
        PRIVATE
        ${TYPICAL_TEST_DEFINITIONS}
)

target_include_directories(
        ${TESTCASE_NAME}
        PRIVATE
        ${TYPICAL_TEST_INC_DIRS}
)

target_link_libraries(
        ${TESTCASE_NAME}
        PRIVATE
        gtest_main
        ${TYPICAL_TEST_LINK_LIBS}
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
