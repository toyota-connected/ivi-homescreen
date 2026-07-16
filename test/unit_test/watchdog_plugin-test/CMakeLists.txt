set(TESTCASE_NAME "homescreen_watchdog_plugin_ut_test_driver")
set(TESTCASE_CC test_case_watchdog_plugin.cc)

# Do NOT define BUILD_WATCHDOG here — all #if BUILD_WATCHDOG branches are dead,
# so no Watchdog::getInstance() is ever called from these tests.
add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_include_directories(${TESTCASE_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/test/unit_test/unit_test_utils
)
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(NAME ${TESTCASE_NAME} COMMAND ${TESTCASE_NAME})
