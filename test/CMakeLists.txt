# required for all test targets
find_package(Threads REQUIRED)

if (BUILD_BACKEND_WAYLAND_EGL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EGL egl glesv2 IMPORTED_TARGET REQUIRED)
endif ()

add_subdirectory(unit_test)
add_subdirectory(integration)
# test/fuzz is added from the top-level CMakeLists instead: the fuzz targets
# need no gtest and no golden images, so they must not depend on
# BUILD_UNIT_TESTS being on as well.
