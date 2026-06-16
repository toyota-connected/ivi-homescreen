set(TESTCASE_NAME "pixelSwizzleTest")

# Header-only test — pixel_swizzle.h has no project dependencies.
# Skip TYPICAL_TEST_SOURCES (the full embedder TU set) to keep the
# build fast.
add_executable(${TESTCASE_NAME} test_case_pixel_swizzle.cc)

add_sanitizers(${TESTCASE_NAME})

target_include_directories(${TESTCASE_NAME}
        PRIVATE
        ${PROJECT_SOURCE_DIR}/shell
)

target_link_libraries(${TESTCASE_NAME}
        PRIVATE
        gtest_main
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
