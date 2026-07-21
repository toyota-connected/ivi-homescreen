# Lean unit test for the ihs_shared platform-view surface. Unlike the typical
# test cases it does NOT compile the shell (TYPICAL_TEST_SOURCES): it links
# libihs_shared directly and drives the exported C ABI with a mock host, so it
# needs no backend, compositor, or app bundle and runs anywhere.
set(TESTCASE_NAME "homescreen_ihs_pv_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_ihs_pv.cc
)

add_sanitizers(${TESTCASE_NAME})

target_link_libraries(${TESTCASE_NAME}
        PRIVATE
        gtest_main
        ivi_homescreen::ihs_shared
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
