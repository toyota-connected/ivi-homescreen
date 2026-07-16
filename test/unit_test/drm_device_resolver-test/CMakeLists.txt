set(TESTCASE_NAME "homescreen_drm_device_resolver_ut_test_driver")
set(TESTCASE_CC test_case_drm_device_resolver.cc)

# drm_device_resolver.cc is only compiled into the shell target when a DRM
# backend is selected, so it is absent from homescreen_ut_shell in a standard
# Wayland-EGL unit-test configure.  It also requires the drm-cxx CMake target
# (which provides the <drm-cxx/core/device.hpp> include path via BUILD_INTERFACE)
# and libdrm pkg-config.  Both are only available when drm_kms.cmake has been
# included (i.e. BUILD_BACKEND_DRM_KMS_EGL or BUILD_BACKEND_DRM_KMS_VULKAN).
#
# TODO(phase3): Inject a fake dri_root path into ResolveDrmDevice so the
# ListCardNodes() / ProbeCard() branches (auto-discovery of /dev/dri/card*)
# can be exercised without real DRM hardware.  See drm_device_resolver.h for
# the function signature and the Phase 3 plan for the seam-injection design.
# Until then, only the explicit-device early-return branch is covered here,
# and only when the drm-cxx target has been brought into scope.

if (NOT TARGET drm-cxx)
  message(
    STATUS
    "drm-cxx target not found (DRM backend not configured) — "
    "skipping ${TESTCASE_NAME}")
  return()
endif ()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM_RESOLVER_LIBDRM REQUIRED IMPORTED_TARGET libdrm)

add_executable(
  ${TESTCASE_NAME}
  ${TESTCASE_CC}
  ${PROJECT_SOURCE_DIR}/shell/display/drm_device_resolver.cc
)
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(
  ${TESTCASE_NAME}
  PRIVATE gtest_main
          homescreen_ut_shell
          drm-cxx
          PkgConfig::DRM_RESOLVER_LIBDRM
)

add_test(NAME ${TESTCASE_NAME} COMMAND ${TESTCASE_NAME})
