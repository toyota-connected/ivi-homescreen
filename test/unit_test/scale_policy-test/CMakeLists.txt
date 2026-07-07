# Fractional-scale rounding policy — pure header, no shell sources needed.
set(TESTCASE_NAME "homescreen_scale_policy_ut_test_driver")
set(TESTCASE_CC test_case_scale_policy.cc)

add_executable(
        ${TESTCASE_NAME}
        ${TESTCASE_CC}
)

add_sanitizers(${TESTCASE_NAME})

target_include_directories(
        ${TESTCASE_NAME}
        PRIVATE
        ${PROJECT_SOURCE_DIR}/shell
)

target_link_libraries(
        ${TESTCASE_NAME}
        PRIVATE
        gtest_main
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
