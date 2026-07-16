# shutdown_flag-test: covers the `running` volatile sig_atomic_t defined in
# shell/app.cc.  app.cc is already part of TYPICAL_TEST_SOURCES (it is in the
# unconditional shell target), so no extra sources are needed.
set(TESTCASE_NAME "homescreen_shutdown_flag_ut_test_driver")
set(TESTCASE_CC test_case_shutdown_flag.cc)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
