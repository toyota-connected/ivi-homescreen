# Integration tests require external resources (a live Wayland compositor for
# app-test and flutter_view-test; a Flutter app bundle for texture-test).  They
# cannot run in a headless CI unit-test-coverage job and are therefore excluded
# by default.
#
# Enable with:
#   cmake -DBUILD_INTEGRATION_TESTS=ON ...
#
# Do NOT add these targets to the unit-test-coverage CI job.

option(BUILD_INTEGRATION_TESTS "Build integration test targets" OFF)

if (NOT BUILD_INTEGRATION_TESTS)
    return()
endif ()

message(STATUS "Integration tests enabled (BUILD_INTEGRATION_TESTS=ON)")

# Integration tests reuse homescreen_ut_shell, the pre-built OBJECT library
# produced by test/unit_test/CMakeLists.txt.  That directory is processed by
# add_subdirectory(unit_test) in the parent CMakeLists before this one, so
# the target is always defined when we get here.
if (NOT TARGET homescreen_ut_shell)
    message(FATAL_ERROR
        "homescreen_ut_shell not found. "
        "Ensure add_subdirectory(unit_test) precedes add_subdirectory(integration) "
        "in test/CMakeLists.txt.")
endif ()

add_subdirectory(app-test)
add_subdirectory(flutter_view-test)
add_subdirectory(texture-test)
