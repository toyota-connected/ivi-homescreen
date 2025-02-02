# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "homescreen_gl_process_resolver_ut_test_driver")
set(TESTCASE_CC test_case_gl_process_resolver.cc)
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")

# Basically, the following statements need not be modified
add_executable(
        ${TESTCASE_NAME}
        ${TYPICAL_TEST_SOURCES}
        ${TESTCASE_CC}
        ${PROJECT_SOURCE_DIR}/shell/backend/gl_process_resolver.cc
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
