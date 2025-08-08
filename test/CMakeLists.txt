# required for all test targets
find_package(Threads REQUIRED)

if (BUILD_BACKEND_WAYLAND_EGL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EGL egl glesv2 IMPORTED_TARGET REQUIRED)
endif ()

add_subdirectory(unit_test)
