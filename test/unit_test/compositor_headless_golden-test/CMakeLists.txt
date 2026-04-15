# Compositor-mode parity gate against the OSMesa headless backend.
# Builds only when both flags are on; the existing test infrastructure
# already requires BUILD_BACKEND_HEADLESS_EGL.
if (NOT BUILD_COMPOSITOR)
    return()
endif ()
if (NOT BUILD_BACKEND_HEADLESS_EGL)
    return()
endif ()

set(TESTCASE_NAME "homescreen_compositor_headless_golden_ut_test_driver")
set(TESTCASE_CC test_case_compositor_headless_golden.cc)
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")

if (UNIT_TEST_SAVE_GOLDENS)
    list(APPEND TYPICAL_TEST_DEFINITIONS SAVE_IMAGE_FOR_COMPARISON=1)
else ()
    list(APPEND TYPICAL_TEST_DEFINITIONS SAVE_IMAGE_FOR_COMPARISON=0)
endif ()

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
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
