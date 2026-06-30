#
# drm_cxx_cursor_load — the drm::cursor loader (Theme + Cursor) built in-tree
# from the drm-cxx submodule sources, WITHOUT the display stack.
#
# This mirrors drm-cxx's own `drm-cxx::cursor-load` target (cursor/theme.cpp +
# cursor/cursor.cpp + the vendored X11-free xcursor-mini loader) but defines it
# here so the software backend can resolve XCursor themes and rasterize cursor
# frames into its own ARGB8888 buffers WITHOUT add_subdirectory(drm-cxx) — which
# would run drm-cxx's display configure (pkg_check_modules(gbm REQUIRED), libdrm,
# libdisplay-info, ...) and pull those into the software backend whose whole
# point is "no GBM / no GL / no extra libraries". The loader links nothing
# beyond the standard library (header-only span/expected polyfill under C++17).
#

set(_dcxx "${CMAKE_SOURCE_DIR}/third_party/drm-cxx")

if (NOT EXISTS "${_dcxx}/src/cursor/cursor.cpp")
    message(STATUS
        "drm-cxx cursor-load sources missing; software themed cursor disabled")
    return()
endif ()

# The cursor sources include detail/{expected,span}.hpp, which back
# <tl/expected.hpp> / <tcb/span.hpp> on a pre-C++20 toolchain (this project is
# C++17). Resolve those polyfills the same way drm-cxx does: prefer a system
# package (a Yocto recipe / cross sysroot provides them via find_package), else
# fall back to drm-cxx's checked-out git submodules under third_party/. Never
# fetch — offline / Yocto builds must not touch the network at configure. If a
# polyfill is reachable by neither route the submodule simply is not checked out
# (e.g. a non-recursive clone); disable the themed cursor rather than fail.
set(_cursor_polyfill_targets "")
set(_cursor_polyfill_includes "")

find_package(tl-expected CONFIG QUIET)
if (tl-expected_FOUND)
    list(APPEND _cursor_polyfill_targets tl::expected)
elseif (EXISTS "${_dcxx}/third_party/tl-expected/include/tl/expected.hpp")
    list(APPEND _cursor_polyfill_includes
        "${_dcxx}/third_party/tl-expected/include")
else ()
    message(STATUS
        "tl-expected unavailable (no system package, drm-cxx submodule not "
        "checked out); software themed cursor disabled")
    return()
endif ()

find_package(tcb-span CONFIG QUIET)
if (tcb-span_FOUND)
    list(APPEND _cursor_polyfill_targets tcb::span)
elseif (EXISTS "${_dcxx}/third_party/tcb-span/include/tcb/span.hpp")
    list(APPEND _cursor_polyfill_includes
        "${_dcxx}/third_party/tcb-span/include")
else ()
    message(STATUS
        "tcb-span unavailable (no system package, drm-cxx submodule not "
        "checked out); software themed cursor disabled")
    return()
endif ()

# C is for the vendored .xcursor loader (xcursor_file.c).
enable_language(C)

add_library(drm_cxx_cursor_load STATIC
        "${_dcxx}/src/cursor/theme.cpp"
        "${_dcxx}/src/cursor/cursor.cpp"
        "${_dcxx}/third_party/xcursor-mini/xcursor_file.c")
add_library(drm-cxx::cursor-load ALIAS drm_cxx_cursor_load)

set_target_properties(drm_cxx_cursor_load PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(drm_cxx_cursor_load PUBLIC cxx_std_17)

# Inherit the project toolchain (stdlib selection — e.g. -stdlib=libc++ on
# clang) so this in-tree lib's C++ ABI matches the rest of the build. Without
# it a clang+libc++ build compiles these sources against the default libstdc++
# and the link fails with std::__cxx11 / std::__1 symbol mismatches (the
# software backend pulls this in directly when no DRM backend supplies the full
# drm-cxx). Mirrors drm-cxx's own target, which links toolchain::toolchain.
target_link_libraries(drm_cxx_cursor_load PRIVATE toolchain::toolchain)

# Vendored submodule sources: suppress their warnings under ivi's strict flags.
target_compile_options(drm_cxx_cursor_load PRIVATE -w)

target_include_directories(drm_cxx_cursor_load
        PUBLIC
            # src/ resolves the public "cursor/cursor.hpp" + "detail/..." paths.
            "${_dcxx}/src"
            # Polyfill include roots (empty when a system package supplies the
            # imported target below instead).
            ${_cursor_polyfill_includes}
        PRIVATE
            # cursor.cpp includes the vendored "xcursor.h".
            "${_dcxx}/third_party/xcursor-mini")

# System-provided polyfills carry their own SYSTEM include dirs via the
# imported target; link them so both this lib and its consumers see the headers.
if (_cursor_polyfill_targets)
    target_link_libraries(drm_cxx_cursor_load PUBLIC ${_cursor_polyfill_targets})
endif ()
