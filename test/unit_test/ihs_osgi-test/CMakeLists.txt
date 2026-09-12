# Lean unit test for the ihs_shared OSGi handshake surface. Like ihs_pv-test it
# does NOT compile the shell: it links libihs_shared directly and drives the
# exported C ABI with a mock host, so it needs no Dart VM, engine, or display.
set(TESTCASE_NAME "homescreen_ihs_osgi_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_ihs_osgi.cc
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
