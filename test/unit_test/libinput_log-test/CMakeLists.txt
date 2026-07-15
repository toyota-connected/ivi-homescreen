# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "homescreen_libinput_log_ut_test_driver")
set(TESTCASE_CC test_case_libinput_log.cc)
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")

# libinput_log_bridge.h includes <libinput.h>, so this test requires the
# libinput development headers (libinput-dev / libinput-devel).  When the
# software backend is built those headers arrive transitively through
# PkgConfig::SW_LIBINPUT, but the unit-test configure does not enable that
# backend.  Detect libinput here directly and skip the target gracefully if
# the headers are absent.
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(UT_LIBINPUT libinput IMPORTED_TARGET)
endif ()

if (NOT UT_LIBINPUT_FOUND)
    message(STATUS "libinput headers not found — skipping ${TESTCASE_NAME}")
    return()
endif ()

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
        ${UT_LIBINPUT_INCLUDE_DIRS}
)

target_link_libraries(
        ${TESTCASE_NAME}
        PRIVATE
        gtest_main
        ${TYPICAL_TEST_LINK_LIBS}
        PkgConfig::UT_LIBINPUT
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
