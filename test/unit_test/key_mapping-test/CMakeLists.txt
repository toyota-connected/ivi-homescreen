set(TESTCASE_NAME "homescreen_key_mapping_ut_test_driver")
set(TESTCASE_CC test_case_key_mapping.cc)

find_package(PkgConfig REQUIRED)
pkg_check_modules(KEY_MAPPING_TEST_XKB REQUIRED IMPORTED_TARGET xkbcommon)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(
  ${TESTCASE_NAME}
  PRIVATE gtest_main
          homescreen_ut_shell
          PkgConfig::KEY_MAPPING_TEST_XKB
)

add_test(NAME ${TESTCASE_NAME} COMMAND ${TESTCASE_NAME})
