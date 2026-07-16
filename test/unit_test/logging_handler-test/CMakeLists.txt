set(TESTCASE_NAME "homescreen_logging_handler_ut_test_driver")
set(TESTCASE_CC test_case_logging_handler.cc)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_include_directories(${TESTCASE_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/test/unit_test/unit_test_utils
)
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(NAME ${TESTCASE_NAME} COMMAND ${TESTCASE_NAME})
