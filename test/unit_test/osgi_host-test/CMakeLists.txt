# Drives the real ihs_osgi_* C surface against a real BridgeRegistry through the
# host table the shell installs -- the only test where both halves of the FFI
# path meet. Links libihs_shared for the forwarders as well as the shell objects
# for the registry and the adapter behind them.
set(TESTCASE_NAME "homescreen_osgi_host_ut_test_driver")
set(TESTCASE_CC test_case_osgi_host.cc;)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME}
        PRIVATE
        gtest_main
        homescreen_ut_shell
        ivi_homescreen::ihs_shared
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
