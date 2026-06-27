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

# C is for the vendored .xcursor loader (xcursor_file.c).
enable_language(C)

add_library(drm_cxx_cursor_load STATIC
        "${_dcxx}/src/cursor/theme.cpp"
        "${_dcxx}/src/cursor/cursor.cpp"
        "${_dcxx}/third_party/xcursor-mini/xcursor_file.c")
add_library(drm-cxx::cursor-load ALIAS drm_cxx_cursor_load)

set_target_properties(drm_cxx_cursor_load PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(drm_cxx_cursor_load PUBLIC cxx_std_17)

# Vendored submodule sources: suppress their warnings under ivi's strict flags.
target_compile_options(drm_cxx_cursor_load PRIVATE -w)

target_include_directories(drm_cxx_cursor_load
        PUBLIC
            # src/ resolves the public "cursor/cursor.hpp" + "detail/..." paths;
            # the polyfill roots back <tl/expected.hpp> / <tcb/span.hpp> on a
            # pre-C++20 toolchain (this project is C++17).
            "${_dcxx}/src"
            "${_dcxx}/subprojects/tl-expected/include"
            "${_dcxx}/subprojects/tcb-span/include"
        PRIVATE
            # cursor.cpp includes the vendored "xcursor.h".
            "${_dcxx}/third_party/xcursor-mini")
