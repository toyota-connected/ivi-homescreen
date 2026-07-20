# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "homescreen_utils_ut_test_driver")
set(TESTCASE_CC test_case_utils.cc)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)
if (BUILD_BACKEND_WAYLAND_EGL)
    target_link_libraries(${TESTCASE_NAME} PRIVATE PkgConfig::EGL)
endif ()

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
