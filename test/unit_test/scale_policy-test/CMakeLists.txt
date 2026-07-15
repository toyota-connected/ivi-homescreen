# Fractional-scale rounding policy — pure header, no shell sources needed. The
# test also includes the wayland-cxx-scanner shared conformance vectors
# (<wl/scale_policy_vectors.hpp>, self-contained, no Wayland link) to assert
# equivalence against the canonical table.
set(TESTCASE_NAME "homescreen_scale_policy_ut_test_driver")
set(TESTCASE_CC test_case_scale_policy.cc)

add_executable(
        ${TESTCASE_NAME}
        ${TESTCASE_CC}
)

add_sanitizers(${TESTCASE_NAME})

target_include_directories(
        ${TESTCASE_NAME}
        PRIVATE
        ${PROJECT_SOURCE_DIR}/shell
        # Shared conformance vectors live in the wcs submodule's framework
        # headers; the vectors header is self-contained (no Wayland link).
        ${PROJECT_SOURCE_DIR}/third_party/wayland-cxx-scanner/include
)

target_link_libraries(
        ${TESTCASE_NAME}
        PRIVATE
        gtest_main
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
