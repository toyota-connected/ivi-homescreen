# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "homescreen_wake_event_fd_ut_test_driver")
set(TESTCASE_CC test_case_wake_event_fd.cc)
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")

# wake_event_fd.cc is only added to the shell target when a libinput- or
# DRM-backed backend is built (BUILD_SOFTWARE_INPUT_LIBINPUT /
# BUILD_BACKEND_DRM_KMS_*).  In the unit-test configuration those backends
# are off, so the source is absent from TYPICAL_TEST_SOURCES.  Add it here
# explicitly so the test can always be compiled and run independently.
set(WAKE_EVENT_FD_SRC ${PROJECT_SOURCE_DIR}/shell/input/wake_event_fd.cc)

# Basically, the following statements need not be modified
add_executable(
        ${TESTCASE_NAME}
        ${TYPICAL_TEST_SOURCES}
        ${WAKE_EVENT_FD_SRC}
        ${TESTCASE_CC}
)

add_sanitizers(${TESTCASE_NAME})

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
