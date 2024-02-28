# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "homescreen_flutter_view_ut_test_driver")
set(TESTCASE_CC test_case_flutter_view.cc)

list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")

# Basically, the following statements need not be modified
add_executable(
        ${TESTCASE_NAME}
        ${TYPICAL_TEST_SOURCES}
        ${TESTCASE_CC}
)

add_sanitizers(${TESTCASE_NAME})

if (IPO_SUPPORT_RESULT)
    set_property(TARGET ${TESTCASE_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${TESTCASE_NAME} PRIVATE ${CONTEXT_COMPILE_OPTIONS})
    target_link_options(${TESTCASE_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fuse-ld=lld -lc++ -lc++abi -lgcc -lc -lm -v>)
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
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
