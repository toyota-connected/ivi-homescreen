# test-case specific settings
set(TESTCASE_NAME "homescreen_text_input_ut_test_driver")
set(TESTCASE_CC test_case_text_input.cc)

# homescreen_ut_shell is an OBJECT library so its .o files are expanded
# directly into this executable's link command — identical to the old
# ${TYPICAL_TEST_SOURCES} pattern.  This avoids the circular-dep ordering
# issue between libflutter.a and libplatform_homescreen.a that a STATIC
# archive would cause (see test/unit_test/CMakeLists.txt for details).
add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
