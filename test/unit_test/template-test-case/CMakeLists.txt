# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "templateTest")
set(TESTCASE_CC test_case.cc)

# Basically, the following statements need not be modified
add_executable(
        ${TESTCASE_NAME}
        ${TYPICAL_TEST_SOURCES}
        ${TESTCASE_CC}
)

add_sanitizers(${TESTCASE_NAME})

# TYPICAL_TEST_SOURCES includes the generated protocol headers; they are attached
# to the shell target by ivi_wayland_protocols(), so without this the test only
# builds when the shell happened to be built first.
if (COMMAND ivi_wayland_protocol_deps)
    ivi_wayland_protocol_deps(${TESTCASE_NAME})
endif ()

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
