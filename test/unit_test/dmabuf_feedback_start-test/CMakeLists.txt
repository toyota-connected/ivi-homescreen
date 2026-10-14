# Where the wayland_vulkan zero-copy path gets the compositor's dma-buf
# modifiers, against a mock compositor (CI, no GPU, no compositor).
#
# Like lease_client-test, it builds only the unit under test (a header) and
# the mock, not the shell.
set(TESTCASE_NAME "homescreen_dmabuf_feedback_start_ut_test_driver")

find_package(PkgConfig REQUIRED)
pkg_check_modules(WL_SERVER REQUIRED IMPORTED_TARGET wayland-server)

add_executable(
        ${TESTCASE_NAME}
        test_case_dmabuf_feedback_start.cc
)

add_sanitizers(${TESTCASE_NAME})

target_include_directories(
        ${TESTCASE_NAME}
        PRIVATE
        ${PROJECT_SOURCE_DIR}/shell
        ${CMAKE_BINARY_DIR}/shell
        ${CMAKE_BINARY_DIR}/shell/wayland-protocols
)

# The generated linux-dmabuf client header is attached to the shell target;
# depend on the same codegen so this target builds standalone.
add_dependencies(${TESTCASE_NAME} wlcxxgen_wayland_protocols_linux_dmabuf_client_hpp)

target_link_libraries(
        ${TESTCASE_NAME}
        PRIVATE
        gtest_main
        wayland-cxx::wayland-cxx
        PkgConfig::WAYLAND_CXX_RUNTIME
        PkgConfig::WL_SERVER
        Threads::Threads
        toolchain::toolchain
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
