# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "homescreen_gl_process_resolver_ut_test_driver")
set(TESTCASE_CC test_case_gl_process_resolver.cc)
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")

# gl_process_resolver.cc is already in homescreen_ut_shell via TYPICAL_TEST_SOURCES.
add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
