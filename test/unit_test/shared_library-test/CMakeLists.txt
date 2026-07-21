# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "homescreen_shared_library_ut_test_driver")
set(TESTCASE_CC test_case_shared_library.cc)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell PkgConfig::GIO)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
