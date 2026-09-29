#
# DRM/KMS backend build wiring.
#
# drm-cxx is consumed as a git submodule under third_party/drm-cxx and built
# in-tree via add_subdirectory. System deps (libdrm, gbm, libinput, libudev)
# are resolved through pkg-config.
#

if (NOT BUILD_BACKEND_DRM_KMS_EGL AND NOT BUILD_BACKEND_DRM_KMS_VULKAN)
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
set(DRM_CXX_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(DRM_CXX_BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(DRM_CXX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(DRM_CXX_INSTALL          OFF CACHE BOOL "" FORCE)
set(DRM_CXX_VULKAN           OFF CACHE BOOL "" FORCE)

# Force-on the drm-cxx modules ivi-homescreen UNCONDITIONALLY depends on,
# so a missing system dep fails the configure step here rather than
# silently auto-disabling the module and producing a binary that crashes
# at first use. Today only DRM_CXX_SESSION qualifies: drm::session::Seat
# is consumed unconditionally in backend/drm_kms_egl/drm_session.cc.
set(DRM_CXX_SESSION ON CACHE BOOL "" FORCE)

# Leave AUTO (don't FORCE) on the drm-cxx modules whose ivi-homescreen
# consumers are themselves gated on the same system dep — shell/CMakeLists
# .txt probes libxcursor (gates drm_cursor.cc + sets HAVE_DRM_CURSOR=1) and
# blend2d (gates drm_capture.cc + sets HAVE_DRM_CAPTURE=1), silently
# dropping the source file when the dep is absent. Mirroring AUTO here
# matches that optionality: drm-cxx's cursor + capture modules build when
# libxcursor + blend2d are present, silently drop out together when not.
# Forcing ON here would break the configure for environments that don't
# have libxcursor / blend2d available (CI runners without libblend2d-dev,
# minimal embedded targets that don't want either feature).

# Force-off the optional drm-cxx modules ivi-homescreen does NOT use, so
# we don't transitively pull in their deps (e.g. csd needs Blend2D
# independently of capture; gstreamer support pulls in GStreamer dev
# packages we don't need on the DRM path; streams is a Tegra/L4T-only
# concern). Flip these to ON if/when a feature lands that consumes them.
set(DRM_CXX_CSD       OFF CACHE BOOL "" FORCE)
set(DRM_CXX_GSTREAMER OFF CACHE BOOL "" FORCE)
set(DRM_CXX_STREAMS   OFF CACHE BOOL "" FORCE)

# CMP0079 NEW lets us attach link libraries to a target created in a
# different directory (drm-cxx's own CMakeLists, processed below).
cmake_policy(SET CMP0079 NEW)

add_subdirectory(${_drm_cxx_src} ${CMAKE_BINARY_DIR}/third_party/drm-cxx EXCLUDE_FROM_ALL)

# drm-cxx's Blend2D verdict, hoisted out of its directory scope.
#
# drm::capture (capture/snapshot.cpp, capture/png.cpp) is compiled only when
# DRM_CXX_HAS_BLEND2D, so that variable -- not a second probe -- is what decides
# whether drm::capture::snapshot and write_png exist to link against.
#
# The shell and the vkms test used to run their own find_package(blend2d), and
# the two sides can disagree. find_package in package mode is not constrained by
# the toolchain's CMAKE_FIND_ROOT_PATH_MODE_LIBRARY / _INCLUDE, so a cross
# configure resolves the *host* package and reports found, while drm-cxx --
# which also requires <blend2d/blend2d.h> to be reachable -- correctly decides
# not to build the module. The build then compiles drm_capture.cc and fails at
# link on both symbols. Reading the verdict makes disagreement impossible.
get_directory_property(IVI_DRM_HAVE_CAPTURE
        DIRECTORY "${_drm_cxx_src}" DEFINITION DRM_CXX_HAS_BLEND2D)
if (NOT IVI_DRM_HAVE_CAPTURE)
    set(IVI_DRM_HAVE_CAPTURE 0)
endif ()

# Cross-check the verdict against what drm-cxx actually compiled.
# DRM_CXX_HAS_BLEND2D is drm-cxx's own internal variable, so a submodule bump
# could rename it and get_directory_property would quietly return empty --
# disabling capture on a tree that has it, and taking two pixel tests with it.
# Fail the configure instead.
get_target_property(_drmcxx_srcs drm-cxx SOURCES)
set(_drmcxx_capture 0)
if (_drmcxx_srcs MATCHES "capture/snapshot\\.cpp")
    set(_drmcxx_capture 1)
endif ()
if (NOT _drmcxx_capture EQUAL IVI_DRM_HAVE_CAPTURE)
    message(FATAL_ERROR
        "drm-cxx capture gate mismatch: DRM_CXX_HAS_BLEND2D reads "
        "'${IVI_DRM_HAVE_CAPTURE}' but capture/snapshot.cpp is "
        "${_drmcxx_capture} in drm-cxx's sources. That variable belongs to "
        "drm-cxx; check whether a submodule bump renamed it.")
endif ()
unset(_drmcxx_srcs)
unset(_drmcxx_capture)

message(STATUS "drm::capture available .. ${IVI_DRM_HAVE_CAPTURE}")

# Treat drm-cxx as a system library (vendored submodule we keep pristine):
# mark its interface headers SYSTEM so they're -isystem for consumers, and
# silence its own-source warnings (-Wsign-conversion, -Wc++20-extensions, …).
if (TARGET drm-cxx)
    get_target_property(_drmcxx_inc drm-cxx INTERFACE_INCLUDE_DIRECTORIES)
    if (_drmcxx_inc)
        set_target_properties(drm-cxx PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_drmcxx_inc}")
    endif ()
    target_compile_options(drm-cxx PRIVATE -w)
endif ()

# drm-cxx is built as a sub-project and doesn't automatically inherit the
# `toolchain` INTERFACE library that applies `-stdlib=libc++` on clang
# builds. Without this, drm-cxx compiles against libstdc++ and the main
# ivi-homescreen binary (libc++) fails to link its STL symbols.
if (TARGET toolchain)
    target_link_libraries(drm-cxx PRIVATE toolchain::toolchain)

    # drm-cxx transitively pulls in fmt via FetchContent when the toolchain
    # lacks std::print. fmt builds with the default compiler stdlib
    # (libstdc++), so its objects won't satisfy the libc++ symbols the main
    # binary needs. Attaching `toolchain` as a link dep would drag it into
    # fmt's install EXPORT set — fmt forces FMT_INSTALL=ON — so copy just
    # the compile/link flags instead, leaving fmt's export set untouched.
    if (TARGET fmt)
        get_target_property(_tc_copts toolchain INTERFACE_COMPILE_OPTIONS)
        get_target_property(_tc_lopts toolchain INTERFACE_LINK_OPTIONS)
        if (_tc_copts)
            target_compile_options(fmt PRIVATE ${_tc_copts})
        endif ()
        if (_tc_lopts)
            target_link_options(fmt PRIVATE ${_tc_lopts})
        endif ()
    endif ()
endif ()