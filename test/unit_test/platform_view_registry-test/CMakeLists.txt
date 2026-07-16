# Exercises the PlatformViewRegistry lifecycle, factory path, and
# registry-owned instance destruction. The registry references the engine
# state only under BUILD_COMPOSITOR (compositor-surface safety nets), and the
# test constructs it with a null state, so it builds either way.

set(TESTCASE_NAME "homescreen_platform_view_registry_ut_test_driver")
set(TESTCASE_CC test_case_platform_view_registry.cc)
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")

# The registry is a platform_homescreen source, not one of the shell sources
# TYPICAL_TEST_SOURCES pulls in, so compile it directly into the driver.
add_executable(
        ${TESTCASE_NAME}
        ${TYPICAL_TEST_SOURCES}
        ${PROJECT_SOURCE_DIR}/shell/platform/homescreen/platform_views/platform_view_registry.cc
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
