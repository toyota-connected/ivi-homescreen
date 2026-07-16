# hexdump-test: covers IosFlagSaver (RAII ios-flag save/restore) and the
# CustomHexdump<RowSize,ShowAscii> streaming template defined in shell/hexdump.h.
# Both are pure header-only; no shell sources, no Wayland/EGL deps.
set(TESTCASE_NAME "homescreen_hexdump_ut_test_driver")
set(TESTCASE_CC test_case_hexdump.cc)

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
