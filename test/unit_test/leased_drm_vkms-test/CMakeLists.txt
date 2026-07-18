# T2 — kernel-real DRM lease on vkms (no compositor).
#
# Unlike the T1 lease-client tier, this one follows the standard
# TYPICAL_TEST_SOURCES pattern: it drives DrmDumbSink, which is genuinely wired
# into the shell (vsync provider, profiling, task runner), so building it
# standalone would mean re-listing half the shell by hand.
#
# Every case skips (not fails) without vkms + DRM master, so this is safe to
# build and run unprivileged and in CI; it only does real work under `sudo`.
set(TESTCASE_NAME "homescreen_leased_drm_vkms_ut_test_driver")
set(TESTCASE_CC test_case_leased_drm_vkms.cc;)

add_executable(
        ${TESTCASE_NAME}
        ${TYPICAL_TEST_SOURCES}
        ${TESTCASE_CC}
)

add_sanitizers(${TESTCASE_NAME})

# TYPICAL_TEST_SOURCES now carries lease_client.cc, which includes the generated
# drm-lease client header. The header is attached to the shell target by
# ivi_wayland_protocols(), so without this the test only builds when the shell
# happened to be built first.
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
