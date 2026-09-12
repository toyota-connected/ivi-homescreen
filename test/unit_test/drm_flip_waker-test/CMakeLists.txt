# The #367 waker, driven through the real DrmDisplay against vkms.
#
# Follows the TYPICAL_TEST_SOURCES pattern: DrmDisplay is wired into the shell
# (seat, output provider, drm-cxx), so building it standalone would mean
# re-listing half the shell by hand -- the same reason leased_drm_vkms-test
# does. drm_flip_wake-test covers the mechanism without any of that.
#
# Both cases skip (not fail) without vkms + DRM master, so this is safe to
# build and run unprivileged and in CI.
set(TESTCASE_NAME "homescreen_drm_flip_waker_ut_test_driver")
set(TESTCASE_CC test_case_drm_flip_waker.cc;)

add_executable(
        ${TESTCASE_NAME}
        ${TYPICAL_TEST_SOURCES}
        ${TESTCASE_CC}
)

add_sanitizers(${TESTCASE_NAME})

if (TARGET wlcxxgen_wayland_protocols_drm_lease_v1_client_hpp)
    add_dependencies(${TESTCASE_NAME}
            wlcxxgen_wayland_protocols_drm_lease_v1_client_hpp)
endif ()

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
