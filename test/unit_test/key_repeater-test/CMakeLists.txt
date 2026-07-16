# key_repeater-test: covers KeyRepeater (timerfd-driven key auto-repeat).
#
# Both key_repeater.cc and xkb_keyboard.cc are conditionally compiled in the
# shell target (gated on BUILD_SOFTWARE_INPUT_LIBINPUT / DRM backends), so
# they are absent from TYPICAL_TEST_SOURCES.  Add them explicitly.
find_package(PkgConfig REQUIRED)
pkg_check_modules(KEY_REPEATER_TEST_XKB REQUIRED IMPORTED_TARGET xkbcommon)

set(TESTCASE_NAME "homescreen_key_repeater_ut_test_driver")
set(TESTCASE_CC test_case_key_repeater.cc)

# key_repeater.cc and xkb_keyboard.cc are included in homescreen_ut_shell.
add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell PkgConfig::KEY_REPEATER_TEST_XKB)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
