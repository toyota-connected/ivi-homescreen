set(TESTCASE_NAME "homescreen_drm_backend_vkms_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_drm_backend_vkms.cc
)

add_sanitizers(${TESTCASE_NAME})

# Frame readback needs drm-cxx's capture module, which is AUTO on blend2d --
# absent on a CI runner without libblend2d-dev, where drm::capture::snapshot is
# never compiled and the pixel cases would fail to link. Same probe the shell
# uses to gate its own SIGUSR1 snapshot.
find_package(blend2d CONFIG QUIET)
if (blend2d_FOUND)
    target_compile_definitions(${TESTCASE_NAME} PRIVATE IHS_TEST_HAVE_CAPTURE=1)
else ()
    message(STATUS "blend2d not found; DRM compositor frame-readback cases disabled")
endif ()

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
