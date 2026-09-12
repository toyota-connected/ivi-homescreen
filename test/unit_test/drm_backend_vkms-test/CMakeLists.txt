set(TESTCASE_NAME "homescreen_drm_backend_vkms_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_drm_backend_vkms.cc
)

add_sanitizers(${TESTCASE_NAME})

target_link_libraries(${TESTCASE_NAME}
        PRIVATE
        gtest
        homescreen_ut_shell
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
