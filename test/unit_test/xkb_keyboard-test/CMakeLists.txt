# xkb_keyboard-test: covers XkbKeyboard (libxkbcommon wrapper).
#
# xkb_keyboard.cc is conditionally compiled in the shell target (only when
# BUILD_SOFTWARE_INPUT_LIBINPUT or a DRM backend is enabled) and is therefore
# absent from TYPICAL_TEST_SOURCES in a default Wayland-EGL unit-test build.
# Add it explicitly, identical to the wake_event_fd-test pattern.
#
# PkgConfig::XKBCOMMON_PLATFORM is directory-scoped (created by
# shell/platform/homescreen/CMakeLists.txt) and may not be visible here.
# Re-probe xkbcommon directly with a local module name.
find_package(PkgConfig REQUIRED)
pkg_check_modules(XKB_KEYBOARD_TEST_XKB REQUIRED IMPORTED_TARGET xkbcommon)

set(TESTCASE_NAME "homescreen_xkb_keyboard_ut_test_driver")
set(TESTCASE_CC test_case_xkb_keyboard.cc)

set(XKB_KEYBOARD_SRC ${PROJECT_SOURCE_DIR}/shell/input/xkb_keyboard.cc)

add_executable(
        ${TESTCASE_NAME}
        ${TYPICAL_TEST_SOURCES}
        ${XKB_KEYBOARD_SRC}
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
        PkgConfig::XKB_KEYBOARD_TEST_XKB
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
