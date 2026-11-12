# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "homescreen_drm_backend_vkms_vk_ut_test_driver")
set(TESTCASE_CC test_case_drm_backend_vkms_vk.cc)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

# The turned-picture cases read the CRTC back through drm-cxx's capture module,
# which is only compiled where drm-cxx found Blend2D. Same gate as the EGL vkms
# fixture.
if (IVI_DRM_HAVE_CAPTURE)
    target_compile_definitions(${TESTCASE_NAME} PRIVATE IHS_TEST_HAVE_CAPTURE=1)
endif ()

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
