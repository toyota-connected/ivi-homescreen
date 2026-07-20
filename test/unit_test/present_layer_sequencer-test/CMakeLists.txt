# test-case specific settings
set(TESTCASE_NAME "homescreen_present_layer_sequencer_ut_test_driver")
set(TESTCASE_CC test_case_present_layer_sequencer.cc)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
