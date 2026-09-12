# The dev.osgi/bridge channel handler. Driven through a stub BinaryMessenger
# with StandardMethodCodec-encoded messages rather than by calling into the
# plugin directly -- HandleMethodCall is private, and going through the codec is
# the better test anyway: the narrowing it has to absorb (a Dart int arriving as
# int32 or int64) only exists on that path.
set(TESTCASE_NAME "homescreen_osgi_bridge_plugin_ut_test_driver")
set(TESTCASE_CC test_case_osgi_bridge_plugin.cc;)

add_executable(${TESTCASE_NAME} ${TESTCASE_CC})
add_sanitizers(${TESTCASE_NAME})
target_link_libraries(${TESTCASE_NAME} PRIVATE gtest_main homescreen_ut_shell)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
