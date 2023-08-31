# test-case specific settings
# when creating new test-case, you need to change here
set(TESTCASE_NAME "flutter-auto_utils_ut_test_driver")
set(TESTCASE_CC test_case_utils.cc)
# set(TESTCASE_MOCK timer.cc)
# list(FILTER TYPICAL_TEST_SOURCES EXCLUDE REGEX ".*${TESTCASE_MOCK}")
# list(APPEND TYPICAL_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/mock/${TESTCASE_MOCK})
list(REMOVE_ITEM TYPICAL_TEST_DEFINITIONS "ENABLE_PLUGIN_URL_LAUNCHER")

# Basically, the following statements need not be modified
add_executable(
    ${TESTCASE_NAME} 
    ${TYPICAL_TEST_SOURCES}
    ${TESTCASE_CC}
)

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
