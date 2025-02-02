# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "homescreen_shared_library_ut_test_driver")
set(TESTCASE_CC test_case_shared_library.cc)
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")

find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)

# Basically, the following statements need not be modified
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
        PkgConfig::GIO
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
