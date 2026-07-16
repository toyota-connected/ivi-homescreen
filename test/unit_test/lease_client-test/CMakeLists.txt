# T1 — LeaseClient unit tier (CI, no hardware).
#
# Deliberately does NOT use TYPICAL_TEST_SOURCES: the lease client has no
# dependency on the shell, the engine, or a renderer, and compiling the whole
# shell in would drag the Flutter engine into a test that only needs a socket.
# Only the unit under test plus the mock compositor are built here, which is
# also what lets this run on any CI box with no GPU and no compositor.
set(TESTCASE_NAME "homescreen_lease_client_ut_test_driver")

find_package(PkgConfig REQUIRED)
pkg_check_modules(WL_SERVER REQUIRED IMPORTED_TARGET wayland-server)
if (NOT TARGET PkgConfig::DRM)
    pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
endif ()

add_executable(
        ${TESTCASE_NAME}
        ${PROJECT_SOURCE_DIR}/shell/backend/wayland_leased_drm/lease_client.cc
        test_case_lease_client.cc
)

add_sanitizers(${TESTCASE_NAME})

target_include_directories(
        ${TESTCASE_NAME}
        PRIVATE
        ${PROJECT_SOURCE_DIR}/shell
        ${PROJECT_SOURCE_DIR}/shared/include
        ${CMAKE_BINARY_DIR}/shared/include
        ${CMAKE_BINARY_DIR}/shell
)

# The generated drm-lease client header is attached to the shell target by
# ivi_wayland_protocols(); depend on the same codegen so this target can build
# standalone (e.g. `ninja <testcase>` without building the shell first).
add_dependencies(${TESTCASE_NAME} wlcxxgen_wayland_protocols_drm_lease_v1_client_hpp)

target_link_libraries(
        ${TESTCASE_NAME}
        PRIVATE
        gtest_main
        wayland-cxx::wayland-cxx
        PkgConfig::WAYLAND_CXX_RUNTIME
        PkgConfig::WL_SERVER
        PkgConfig::DRM
        ivi_homescreen::ihs_shared
        Threads::Threads
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
