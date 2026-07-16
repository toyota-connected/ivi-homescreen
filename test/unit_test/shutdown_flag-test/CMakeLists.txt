# shutdown_flag-test: covers the `running` volatile sig_atomic_t defined in
# shell/app.cc.  app.cc is already part of TYPICAL_TEST_SOURCES (it is in the
# unconditional shell target), so no extra sources are needed.
set(TESTCASE_NAME "homescreen_shutdown_flag_ut_test_driver")
set(TESTCASE_CC test_case_shutdown_flag.cc)

add_executable(
        ${TESTCASE_NAME}
        ${TYPICAL_TEST_SOURCES}
        ${TESTCASE_CC}
)

add_sanitizers(${TESTCASE_NAME})

target_compile_definitions(
        ${TESTCASE_NAME}
        PRIVATE
        ${TYPICAL_TEST_DEFINITIONS}
)

target_include_directories(
        ${TESTCASE_NAME}
        PRIVATE
        ${TYPICAL_TEST_INC_DIRS}
)

target_link_libraries(
        ${TESTCASE_NAME}
        PRIVATE
        gtest_main
        ${TYPICAL_TEST_LINK_LIBS}
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
