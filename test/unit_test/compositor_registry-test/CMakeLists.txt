# Phase 5: exercise polymorphic Backend register/unregister/resize dispatch.
# Only meaningful when BUILD_COMPOSITOR is on — otherwise the methods don't
# exist on Backend. The test directory is unconditionally added by the
# parent CMakeLists; the test body itself compiles out cleanly when
# BUILD_COMPOSITOR is off because it only references the default no-op
# virtuals we define on Backend in that mode... actually the test
# references the methods directly, so gate the target here.
if (NOT BUILD_COMPOSITOR)
    return()
endif ()

set(TESTCASE_NAME "homescreen_compositor_registry_ut_test_driver")
set(TESTCASE_CC test_case_compositor_registry.cc)
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")

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
