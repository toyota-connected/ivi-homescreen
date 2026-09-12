# The PAGE_FLIP_EVENT drain race behind #367, against a real DRM device.
#
# Lean, like ihs_pv-test: no TYPICAL_TEST_SOURCES. It drives libdrm and an
# eventfd directly rather than any shell type, so it needs no backend,
# compositor, or app bundle and builds wherever libdrm does.
#
# Both cases skip (not fail) without vkms + DRM master, so this is safe to
# build and run unprivileged and in CI. vkms usually grants master implicitly
# to the first opener:
#
#   sudo modprobe vkms
set(TESTCASE_NAME "homescreen_drm_flip_wake_ut_test_driver")

add_executable(${TESTCASE_NAME}
        test_case_drm_flip_wake.cc
)

add_sanitizers(${TESTCASE_NAME})

find_package(Threads REQUIRED)

target_link_libraries(${TESTCASE_NAME}
        PRIVATE
        gtest_main
        PkgConfig::DRM
        Threads::Threads
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
