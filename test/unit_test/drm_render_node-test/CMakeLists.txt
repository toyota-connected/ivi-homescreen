# drm_render_node::Of, the render node behind a KMS device, checked against
# libdrm's own answer for each card on the machine.
#
# Lean: the header alone plus libdrm for the reference answer. Skips (not
# fails) on a machine with no DRM card.
set(TESTCASE_NAME "homescreen_drm_render_node_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_drm_render_node.cc
)

add_sanitizers(${TESTCASE_NAME})

target_include_directories(${TESTCASE_NAME}
        PRIVATE
        ${PROJECT_SOURCE_DIR}/shell
)

target_link_libraries(${TESTCASE_NAME}
        PRIVATE
        gtest_main
        PkgConfig::DRM
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
