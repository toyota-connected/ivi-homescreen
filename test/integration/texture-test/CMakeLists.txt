# Integration test — NOT a unit test.
#
# This test creates a live Wayland compositor connection (Display), a running
# Flutter engine (Engine), and expects a valid Flutter app bundle on disk at
# TEST_APP_BUNDLE_PATH.  It cannot run in a headless CI environment without
# those external resources, so it lives here rather than in test/unit_test/.
#
# To run manually:
#   cmake -DUNIT_TEST_APP_BUNDLE=<path/to/bundle> ...
#   ninja homescreen_texture_ut_test_driver
#   ./test/integration/texture-test/homescreen_texture_ut_test_driver
#
# Do NOT add this to the unit-test-coverage CI job.

set(TESTCASE_NAME "homescreen_texture_ut_test_driver")
set(TESTCASE_CC test_case_texture.cc)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_compile_definitions(${TESTCASE_NAME} PRIVATE SOURCE_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
