# When a dma-buf present stops waiting on its wl_surface.frame callback.
#
# Header-only unit under test, so this builds neither the shell nor any
# Wayland protocol binding.
set(TESTCASE_NAME "homescreen_frame_wait_ut_test_driver")

add_executable(
        ${TESTCASE_NAME}
        test_case_frame_wait.cc
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
