set(TESTCASE_NAME "homescreen_output_manager_ut_test_driver")
set(TESTCASE_CC test_case_output_manager.cc)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_include_directories(${TESTCASE_NAME} PRIVATE
    ${PROJECT_SOURCE_DIR}/test/unit_test/unit_test_utils
    $<TARGET_PROPERTY:gmock,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(${TESTCASE_NAME} PRIVATE
    gtest_main
    gmock
    homescreen_ut_shell
)

add_test(NAME ${TESTCASE_NAME} COMMAND ${TESTCASE_NAME})
