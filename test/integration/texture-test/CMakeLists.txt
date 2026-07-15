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

list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")

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
        SOURCE_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
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
