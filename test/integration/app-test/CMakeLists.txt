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

list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_FILE_SELECTOR")
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_GO_ROUTER")
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_DESKTOP_WINDOW_LINUX")


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
