# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "homescreen_wake_event_fd_ut_test_driver")
set(TESTCASE_CC test_case_wake_event_fd.cc)
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")

# wake_event_fd.cc is included in homescreen_ut_shell (it is absent from
# TYPICAL_TEST_SOURCES when libinput/DRM backends are off, so it was added
# to the archive explicitly — see test/unit_test/CMakeLists.txt).
add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
