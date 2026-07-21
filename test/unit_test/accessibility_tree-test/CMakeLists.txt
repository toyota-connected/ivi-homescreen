# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "homescreen_accessibility_tree_ut_test_driver")
set(TESTCASE_CC test_case_accessibility_tree.cc)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_compile_definitions(${TESTCASE_NAME} PRIVATE TEST_SCRATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
