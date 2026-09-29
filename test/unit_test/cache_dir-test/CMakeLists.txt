# The read-only-rootfs degrade (#650).
#
# Links homescreen_ut_shell rather than compiling cache_dir.cc on its own: the
# unit reads Utils::IsSafeBasePath, and shell/utils.h pulls in the Flutter
# include tree, so a standalone target would have to carry it. Same reason
# utils-test does it this way.
set(TESTCASE_NAME "homescreen_cache_dir_ut_test_driver")

add_executable(${TESTCASE_NAME} test_case_cache_dir.cc)
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)
if (BUILD_BACKEND_WAYLAND_EGL)
    target_link_libraries(${TESTCASE_NAME} PRIVATE PkgConfig::EGL)
endif ()

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
