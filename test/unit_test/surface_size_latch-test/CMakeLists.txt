# The cross-thread handoff behind wl_surface.set_opaque_region: a surface size
# written on the Wayland event thread and consumed on the raster thread.
#
# Header-only unit under test, so this builds neither the shell nor any
# Wayland protocol binding -- unlike dmabuf_feedback_start-test, it needs no
# mock compositor.
set(TESTCASE_NAME "homescreen_surface_size_latch_ut_test_driver")

add_executable(
        ${TESTCASE_NAME}
        test_case_surface_size_latch.cc
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
        Threads::Threads
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
