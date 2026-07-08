#
# Wayland C++ binding layer: wayland-cxx-scanner.
#
# Consumed as a git submodule under third_party/wayland-cxx-scanner and built
# in-tree via add_subdirectory. It provides:
#   - wayland-cxx::wayland-cxx — the header-only wl/ framework (CRTP proxies,
#     handlers, registry, keyboard, seat, shipped protocol tables);
#   - wayland_cxx_generate() — generates type-safe C++ protocol headers from XML.
#
# This module is included from the top-level CMakeLists.txt (after compiler.cmake
# so the `toolchain` INTERFACE library exists). It sets up the scanner subproject
# and defines ivi_wayland_protocols()/ivi_wayland_link() for the shell target to
# call once it exists. Mirrors cmake/drm_kms.cmake.
#

if (NOT BUILD_BACKEND_WAYLAND_EGL AND NOT BUILD_BACKEND_WAYLAND_VULKAN)
    return()
endif ()

find_package(PkgConfig REQUIRED)

# Core Wayland runtime libs. libwayland-client + libwayland-cursor + xkbcommon
# are needed by every Wayland backend; wayland-egl only by the EGL backend.
pkg_check_modules(WAYLAND_CXX_RUNTIME REQUIRED IMPORTED_TARGET
    wayland-client wayland-cursor xkbcommon)
if (BUILD_BACKEND_WAYLAND_EGL)
    pkg_check_modules(WAYLAND_CXX_EGL REQUIRED IMPORTED_TARGET wayland-egl)
endif ()

# Protocol XML base — xdg-shell + presentation-time ship with wayland-protocols.
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.20)
pkg_get_variable(_wlcxx_proto_base wayland-protocols pkgdatadir)
set(IVI_WL_PROTOCOLS_BASE "${_wlcxx_proto_base}" CACHE INTERNAL "wayland-protocols pkgdatadir")

# Core Wayland protocol XML (wl_seat / wl_keyboard / …) ships with
# wayland-scanner. The generated wayland_client.hpp backs wl::KeyboardHandler;
# its core wl_interface tables come from libwayland (no --emit-interface-tables).
pkg_get_variable(_wlcxx_core_base wayland-scanner pkgdatadir)
set(IVI_WL_CORE_XML "${_wlcxx_core_base}/wayland.xml" CACHE INTERNAL "core wayland.xml")

# --- Shell client options --------------------------------------------------
# xdg + agl on by default; ivi + simple are opt-in.
option(ENABLE_XDG_CLIENT          "Enable XDG shell client"               ON)
option(ENABLE_AGL_SHELL_CLIENT    "Enable AGL shell client"               ON)
option(ENABLE_IVI_SHELL_CLIENT    "Enable ivi-shell client"               OFF)
option(ENABLE_SIMPLE_SHELL_CLIENT "Enable RDK/Westeros simple_shell client" OFF)

# --- Scanner subproject -------------------------------------------------------
set(_wlcxx_src "${CMAKE_SOURCE_DIR}/third_party/wayland-cxx-scanner")
set(IVI_WL_CXX_SRC "${_wlcxx_src}" CACHE INTERNAL "wayland-cxx-scanner source dir")
if (NOT EXISTS "${_wlcxx_src}/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/wayland-cxx-scanner/CMakeLists.txt is missing. Run:\n"
        "    git submodule update --init --recursive third_party/wayland-cxx-scanner")
endif ()

# Suppress the scanner's tests/examples/install and relax -Werror — it is a
# vendored submodule we keep pristine.
set(WAYLAND_CXX_SCANNER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(WAYLAND_CXX_SCANNER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WAYLAND_CXX_SCANNER_INSTALL_TOOL   OFF CACHE BOOL "" FORCE)
set(WAYLAND_CXX_WERROR                 OFF CACHE BOOL "" FORCE)

# IME backend — a *compositor capability* owned by the compositor/BSP layer, not
# a silent project default. Honor an integrator-supplied value; only default for
# local dev, and warn loudly under cross where the recipe should have set it (a
# wrong backend fails silently at runtime as dead text input).
#
# Default to text-input-v3: the protocol the mainstream desktop compositors
# (GNOME/Mutter, KDE) implement, so IME works out of the box for local dev on a
# GNOME session. AGL/weston builds that speak text-input-v1 set it explicitly.
if (NOT WAYLAND_CXX_IME_BACKEND)
    set(WAYLAND_CXX_IME_BACKEND "text-input-v3" CACHE STRING "IME protocol backend")
    if (CMAKE_CROSSCOMPILING)
        message(WARNING
            "WAYLAND_CXX_IME_BACKEND unset; defaulting to 'text-input-v3'. Set it "
            "from the compositor/BSP layer (-DWAYLAND_CXX_IME_BACKEND=...): the IME "
            "backend is a compositor capability and a wrong value fails at runtime "
            "as dead text input, not at build.")
    endif ()
endif ()

# CMP0079 NEW: attach link libs to a target created in another directory.
cmake_policy(SET CMP0079 NEW)

# The scanner's CMakeLists list(APPEND)s its own cmake/ dir to CMAKE_MODULE_PATH,
# but ivi's cmake/ dir is already first and ships same-named modules (git,
# options, compiler). Its include(options)/include(compiler) would therefore
# resolve to ivi's modules — leaving the scanner's wayland-cxx-scanner-warnings
# target and WAYLAND_CXX_IME_DEFINE undefined. Prepend the scanner's module dir
# for the duration of the subproject so it picks up its OWN modules, then restore.
set(_ivi_saved_module_path "${CMAKE_MODULE_PATH}")
list(INSERT CMAKE_MODULE_PATH 0 "${_wlcxx_src}/cmake")

add_subdirectory(${_wlcxx_src} ${CMAKE_BINARY_DIR}/third_party/wayland-cxx-scanner
    EXCLUDE_FROM_ALL)

set(CMAKE_MODULE_PATH "${_ivi_saved_module_path}")

# Under cross-compile the scanner only resolves the host codegen tool inside its
# own BUILD_EXAMPLES block (which we force off). Resolve it ourselves so
# wayland_cxx_generate() has a tool to invoke. Native builds set it to the
# in-tree exe automatically.
if (CMAKE_CROSSCOMPILING AND NOT WAYLAND_CXX_SCANNER_EXECUTABLE)
    find_program(_wlcxx_host_scanner NAMES wayland-cxx-scanner REQUIRED)
    set(WAYLAND_CXX_SCANNER_EXECUTABLE "${_wlcxx_host_scanner}"
        CACHE INTERNAL "host wayland-cxx-scanner codegen tool")
endif ()

# Treat the header-only framework as a system library (pristine submodule).
if (TARGET wayland-cxx)
    get_target_property(_wlcxx_inc wayland-cxx INTERFACE_INCLUDE_DIRECTORIES)
    if (_wlcxx_inc)
        set_target_properties(wayland-cxx PROPERTIES
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_wlcxx_inc}")
    endif ()
endif ()

# Silence the compiled scanner exe/lib (native builds only). Unlike drm-cxx,
# do NOT link toolchain::toolchain onto them: the scanner is a standalone
# codegen tool run *during* this build (not linked into the shell), so its STL
# ABI need not match the main binary. toolchain::toolchain forces the project's
# -stdlib=libc++, which made the freshly-built scanner depend on libc++abi.so.1
# at exec time; on a toolchain that keeps that runtime off the system loader
# path (flutter_workspace's clang) the tool then fails to start and codegen
# aborts. Left on the compiler default (libstdc++, whose .so is always on the
# loader path) the tool runs everywhere. The exe and its static lib stay on the
# same stdlib because neither links the toolchain target.
foreach (_tgt wayland-cxx-scanner wayland-cxx-scanner-lib)
    if (TARGET ${_tgt})
        target_compile_options(${_tgt} PRIVATE -w)
    endif ()
endforeach ()

#
# ivi_wayland_protocols(<target>)
#
# Generate the C++ protocol headers the shell code needs and attach them (dep +
# include dir) to <target>. Only what shell/ actually references:
#   xdg-shell, presentation-time (core-backed: tables from the shipped wl/*.hpp
#   resp. emitted inline), agl-shell, ivi-application, ivi-wm, simpleshell.
#
# Rule: a protocol with a shipped wl/<name>.hpp (xdg, agl, simple) provides its
# wl_interface table inline there, so generate WITHOUT --emit-interface-tables.
# A protocol with no shipped header (presentation-time, ivi-*) needs
# --emit-interface-tables so the generated header is self-contained.
#
function(ivi_wayland_protocols target)
    set(_xdg  "${IVI_WL_PROTOCOLS_BASE}/stable/xdg-shell/xdg-shell.xml")
    set(_pres "${IVI_WL_PROTOCOLS_BASE}/stable/presentation-time/presentation-time.xml")
    set(_bund "${IVI_WL_CXX_SRC}/protocols")
    set(_loc  "${CMAKE_SOURCE_DIR}/shell/wayland/protocols")

    # core wayland — always (wl::KeyboardHandler needs wayland::client::CWlKeyboard);
    # core wl_interface tables come from libwayland, so do NOT emit tables.
    wayland_cxx_generate(PROTOCOL "${IVI_WL_CORE_XML}" MODE client-header
        OUTPUT wayland-protocols/wayland_client.hpp TARGET ${target})

    # presentation-time — always (IVsyncProvider); no shipped header -> emit tables.
    wayland_cxx_generate(PROTOCOL "${_pres}" MODE client-header EMIT_INTERFACE_TABLES
        OUTPUT wayland-protocols/presentation_time_client.hpp TARGET ${target})

    # input-timestamps — always (ivi::InputTimestamps: high-resolution kernel
    # timestamps for the pointer/touch → Flutter path; runtime-optional, the
    # provider no-ops when the compositor doesn't advertise the manager).
    # No shipped header -> emit tables.
    wayland_cxx_generate(PROTOCOL
        "${IVI_WL_PROTOCOLS_BASE}/unstable/input-timestamps/input-timestamps-unstable-v1.xml"
        MODE client-header EMIT_INTERFACE_TABLES
        OUTPUT wayland-protocols/input_timestamps_client.hpp TARGET ${target})

    # viewporter — always (per-surface scaling; stable since wayland-protocols
    # 1.4, so present on Dunfell's 1.20). No shipped header -> emit tables.
    wayland_cxx_generate(PROTOCOL
        "${IVI_WL_PROTOCOLS_BASE}/stable/viewporter/viewporter.xml"
        MODE client-header EMIT_INTERFACE_TABLES
        OUTPUT wayland-protocols/viewporter_client.hpp TARGET ${target})

    # fractional-scale-v1 — always. Vendored: the XML only ships with
    # wayland-protocols >= 1.31, which Dunfell/Ubuntu-20.04 hosts predate.
    # Both protocols are gated at runtime on the compositor advertising the
    # global, so generating unconditionally costs nothing on old stacks.
    wayland_cxx_generate(PROTOCOL "${_loc}/fractional-scale-v1.xml"
        MODE client-header EMIT_INTERFACE_TABLES
        OUTPUT wayland-protocols/fractional_scale_v1_client.hpp TARGET ${target})

    # text-input (IME) — the build-selected backend's client header. The
    # wl::ime backend header (<wl/ime/backends/text_input_v{1,3}.hpp>) includes
    # the generated header by its fixed name with --emit-interface-tables, so it
    # is self-contained. Only the text-input-v1/v3 backends have a generated
    # protocol header; the other IME selections (and 'none') generate nothing.
    if (WAYLAND_CXX_IME_BACKEND STREQUAL "text-input-v3")
        wayland_cxx_generate(PROTOCOL
            "${IVI_WL_PROTOCOLS_BASE}/unstable/text-input/text-input-unstable-v3.xml"
            MODE client-header EMIT_INTERFACE_TABLES
            OUTPUT wayland-protocols/text_input_v3_client.hpp TARGET ${target})
    elseif (WAYLAND_CXX_IME_BACKEND STREQUAL "text-input-v1")
        wayland_cxx_generate(PROTOCOL
            "${IVI_WL_PROTOCOLS_BASE}/unstable/text-input/text-input-unstable-v1.xml"
            MODE client-header EMIT_INTERFACE_TABLES
            OUTPUT wayland-protocols/text_input_v1_client.hpp TARGET ${target})
    endif ()

    if (ENABLE_XDG_CLIENT)
        wayland_cxx_generate(PROTOCOL "${_xdg}" MODE client-header
            OUTPUT wayland-protocols/xdg_shell_client.hpp TARGET ${target})
        target_compile_definitions(${target} PRIVATE ENABLE_XDG_CLIENT=1)
    endif ()
    if (ENABLE_AGL_SHELL_CLIENT)
        wayland_cxx_generate(PROTOCOL "${_bund}/agl-shell.xml" MODE client-header
            OUTPUT wayland-protocols/agl_shell_client.hpp TARGET ${target})
        target_compile_definitions(${target} PRIVATE ENABLE_AGL_SHELL_CLIENT=1)
    endif ()
    if (ENABLE_IVI_SHELL_CLIENT)
        wayland_cxx_generate(PROTOCOL "${_bund}/ivi-application.xml" MODE client-header
            EMIT_INTERFACE_TABLES
            OUTPUT wayland-protocols/ivi_application_client.hpp TARGET ${target})
        wayland_cxx_generate(PROTOCOL "${_loc}/ivi-wm.xml" MODE client-header
            EMIT_INTERFACE_TABLES
            OUTPUT wayland-protocols/ivi_wm_client.hpp TARGET ${target})
        target_compile_definitions(${target} PRIVATE ENABLE_IVI_SHELL_CLIENT=1)
    endif ()
    if (ENABLE_SIMPLE_SHELL_CLIENT)
        wayland_cxx_generate(PROTOCOL "${_bund}/simpleshell.xml" MODE client-header
            OUTPUT wayland-protocols/simple_shell_client.hpp TARGET ${target})
        target_compile_definitions(${target} PRIVATE ENABLE_SIMPLE_SHELL_CLIENT=1)
    endif ()
endfunction()

#
# ivi_wayland_link(<target>)
#
# Link the wl/ framework + libwayland runtime onto <target>.
#
function(ivi_wayland_link target)
    target_link_libraries(${target} PRIVATE
        wayland-cxx::wayland-cxx
        PkgConfig::WAYLAND_CXX_RUNTIME)
    if (TARGET PkgConfig::WAYLAND_CXX_EGL)
        target_link_libraries(${target} PRIVATE PkgConfig::WAYLAND_CXX_EGL)
    endif ()
endfunction()
