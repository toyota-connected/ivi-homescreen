# PlaneFormats against a registry built from synthetic plane capabilities: no
# device, so it runs anywhere drm-cxx is built.
set(TESTCASE_NAME "homescreen_drm_plane_formats_ut_test_driver")

add_executable(${TESTCASE_NAME} test_case_drm_plane_formats.cc)

add_sanitizers(${TESTCASE_NAME})

target_include_directories(${TESTCASE_NAME} PRIVATE
        ${PROJECT_SOURCE_DIR}/shell)

target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main drm-cxx)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
