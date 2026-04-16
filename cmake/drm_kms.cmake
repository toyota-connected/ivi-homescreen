#
# DRM/KMS backend build wiring.
#
# drm-cxx is consumed as a git submodule under third_party/drm-cxx and built
# in-tree via add_subdirectory. System deps (libdrm, gbm, libinput, libudev)
# are resolved through pkg-config.
#

if (NOT BUILD_BACKEND_DRM_KMS_EGL)
    return()
endif ()

find_package(PkgConfig REQUIRED)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(LIBINPUT REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(UDEV REQUIRED IMPORTED_TARGET libudev)

set(_drm_cxx_src "${CMAKE_SOURCE_DIR}/third_party/drm-cxx")
if (NOT EXISTS "${_drm_cxx_src}/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/drm-cxx/CMakeLists.txt is missing. Run:\n"
        "    git submodule update --init --recursive third_party/drm-cxx")
endif ()

# Suppress drm-cxx's tests, examples, install rules, and Vulkan display
# support. ivi-homescreen owns the integration test surface; drm-cxx's
# Vulkan path is orthogonal to the GL renderer this backend drives.
set(DRM_CXX_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(DRM_CXX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(DRM_CXX_INSTALL        OFF CACHE BOOL "" FORCE)
set(DRM_CXX_VULKAN         OFF CACHE BOOL "" FORCE)

# CMP0079 NEW lets us attach link libraries to a target created in a
# different directory (drm-cxx's own CMakeLists, processed below).
cmake_policy(SET CMP0079 NEW)

add_subdirectory(${_drm_cxx_src} ${CMAKE_BINARY_DIR}/third_party/drm-cxx EXCLUDE_FROM_ALL)

# drm-cxx is built as a sub-project and doesn't automatically inherit the
# `toolchain` INTERFACE library that applies `-stdlib=libc++` on clang
# builds. Without this, drm-cxx compiles against libstdc++ and the main
# ivi-homescreen binary (libc++) fails to link its STL symbols.
if (TARGET toolchain)
    target_link_libraries(drm-cxx PRIVATE toolchain::toolchain)
endif ()