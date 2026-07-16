# key_repeater-test: covers KeyRepeater (timerfd-driven key auto-repeat).
#
# Both key_repeater.cc and xkb_keyboard.cc are conditionally compiled in the
# shell target (gated on BUILD_SOFTWARE_INPUT_LIBINPUT / DRM backends), so
# they are absent from TYPICAL_TEST_SOURCES.  Add them explicitly.
find_package(PkgConfig REQUIRED)
pkg_check_modules(KEY_REPEATER_TEST_XKB REQUIRED IMPORTED_TARGET xkbcommon)

set(TESTCASE_NAME "homescreen_key_repeater_ut_test_driver")
set(TESTCASE_CC test_case_key_repeater.cc)

set(KEY_REPEATER_SRCS
        ${PROJECT_SOURCE_DIR}/shell/input/key_repeater.cc
        ${PROJECT_SOURCE_DIR}/shell/input/xkb_keyboard.cc
)

add_executable(
        ${TESTCASE_NAME}
        ${TYPICAL_TEST_SOURCES}
        ${KEY_REPEATER_SRCS}
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
        PkgConfig::KEY_REPEATER_TEST_XKB
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
