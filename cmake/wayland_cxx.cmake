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

# Gate on IVI_WAYLAND_ANY, not on the surface backends: wayland-leased-drm
# needs the scanner + wayland-client to speak drm-lease-v1, even though it
# never creates a wl_surface.
if (NOT IVI_WAYLAND_ANY)
    return()
endif ()

find_package(PkgConfig REQUIRED)

# Core Wayland runtime libs, split by what actually needs them.
#
# wayland-client is required by any backend that opens a wl_display.
#
# wayland-cursor + xkbcommon are *surface* deps: they back wl/cursor.hpp and
# wl/seat.hpp + wl/keyboard.hpp respectively, which only a backend with a
# wl_surface and compositor input focus includes. wayland-leased-drm has
# neither (input comes from evdev), so requiring them here would fail a
# lease-only configure on an image that ships no cursor/xkb libs. The
# wayland-cxx target itself is header-only with no pkg-config deps, so the
# split is safe: the deps are per-header, not per-target.
pkg_check_modules(WAYLAND_CXX_RUNTIME REQUIRED IMPORTED_TARGET wayland-client)
if (IVI_WAYLAND_SURFACE_BACKENDS)
    pkg_check_modules(WAYLAND_CXX_SURFACE REQUIRED IMPORTED_TARGET
        wayland-cursor xkbcommon)
endif ()
if (BUILD_BACKEND_WAYLAND_EGL)
    pkg_check_modules(WAYLAND_CXX_EGL REQUIRED IMPORTED_TARGET wayland-egl)
endif ()

# pkg_get_variable() hands back the variable exactly as the .pc file spells it.
# Unlike the -I/-L flags in the cflags/libs, it is never sysroot-prefixed, so
# when cross-compiling a pkgdatadir such as /usr/share/wayland resolves against
# the *build host* rather than the sysroot. Feeding host XML to the scanner
# generates bindings for the host's libwayland while the code links the
# sysroot's, and the *_SINCE_VERSION guards in the shell then test one version's
# macros against the other version's structs.
#
# Prefer the sysroot copy whenever there is one. The host path usually exists
# too -- that is precisely the failure -- so this cannot be an "only if missing"
# fallback; under CMAKE_SYSROOT the sysroot wins. When the value already points
# inside the sysroot (pkg-config does apply PKG_CONFIG_SYSROOT_DIR when it finds
# the .pc there) the prefixed path does not exist and the value is left alone.
function(_wlcxx_prefer_sysroot var subpath)
    # CMAKE_SYSROOT is the obvious root, but a cross toolchain need not set it.
    # OpenEmbedded's cmake class began emitting it in 4.0 (kirkstone); 3.1
    # (dunfell) names the sysroot through CMAKE_FIND_ROOT_PATH alone. Returning
    # early there left the caller holding an unprefixed pkg-config path -- the
    # build host's copy -- with no diagnostic, which for the core wayland.xml
    # means generating bindings against a libwayland that is not the one being
    # linked. Try every root that could name a sysroot, most specific first.
    set(_roots "${CMAKE_SYSROOT}" ${CMAKE_FIND_ROOT_PATH} "${CMAKE_STAGING_PREFIX}")
    foreach (_root IN LISTS _roots)
        if (NOT _root)
            continue()
        endif ()
        set(_candidate "${_root}${${var}}")
        if (EXISTS "${_candidate}/${subpath}")
            set(${var} "${_candidate}" PARENT_SCOPE)
            return()
        endif ()
    endforeach ()
endfunction()

# Protocol XML base — xdg-shell + presentation-time ship with wayland-protocols.
#
# Surface builds only: a lease-only build reads nothing from wayland-protocols.
# Its core wayland.xml comes from wayland-scanner's pkgdatadir (below) and
# drm-lease-v1.xml is vendored in-tree (the staging XML needs >= 1.22, above our
# 1.20 floor), so requiring the package here would fail a lease-only configure
# on a minimal sysroot over data the build never reads. Every consumer of
# IVI_WL_PROTOCOLS_BASE sits behind the surface-backend gate in
# ivi_wayland_protocols().
if (IVI_WAYLAND_SURFACE_BACKENDS)
    pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.20)
    pkg_get_variable(_wlcxx_proto_base wayland-protocols pkgdatadir)
    _wlcxx_prefer_sysroot(_wlcxx_proto_base "stable/xdg-shell/xdg-shell.xml")
    set(IVI_WL_PROTOCOLS_BASE "${_wlcxx_proto_base}" CACHE INTERNAL "wayland-protocols pkgdatadir")
endif ()

# Core Wayland protocol XML (wl_seat / wl_keyboard / …) ships with
# wayland-scanner. The generated wayland_client.hpp backs wl::KeyboardHandler;
# its core wl_interface tables come from libwayland (no --emit-interface-tables).
# Settable: an integrator whose sysroot layout defeats the probe above can point
# this straight at the right XML. It must describe the libwayland actually being
# linked -- a mismatch is silent when the XML is older (events are compiled out
# with no diagnostic) and a compile error when it is newer.
pkg_get_variable(_wlcxx_core_base wayland-scanner pkgdatadir)
_wlcxx_prefer_sysroot(_wlcxx_core_base "wayland.xml")
set(IVI_WL_CORE_XML "${_wlcxx_core_base}/wayland.xml" CACHE FILEPATH "core wayland.xml")

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
# Rule: a protocol whose shipped wl/<name>.hpp provides its wl_interface table
# inline (xdg, simple) generates WITHOUT --emit-interface-tables. Everything
# else needs --emit-interface-tables so the generated header is self-contained:
# protocols with no shipped header (presentation-time, ivi-*) and agl-shell,
# whose shipped header stopped hand-writing the table in scanner v1.0.0.
#
# Everything except core wayland and drm-lease-v1 is *surface*-only: those
# protocols exist to drive a wl_surface (shells, scaling, dmabuf present,
# vsync, IME, input). wayland-leased-drm has no surface — it connects solely
# to negotiate a DRM lease — so a lease-only build generates just core +
# drm-lease. This also keeps non-lease builds byte-identical: the lease
# codegen is conditional on the option.
#
function(ivi_wayland_protocols target)
    set(_xdg  "${IVI_WL_PROTOCOLS_BASE}/stable/xdg-shell/xdg-shell.xml")
    set(_pres "${IVI_WL_PROTOCOLS_BASE}/stable/presentation-time/presentation-time.xml")
    set(_bund "${IVI_WL_CXX_SRC}/protocols")
    set(_loc  "${CMAKE_SOURCE_DIR}/shell/wayland/protocols")

    # core wayland — always (wl::Registry drives the lease client's globals;
    # wl::KeyboardHandler needs wayland::client::CWlKeyboard on surface builds);
    # core wl_interface tables come from libwayland, so do NOT emit tables.
    wayland_cxx_generate(PROTOCOL "${IVI_WL_CORE_XML}" MODE client-header
        OUTPUT wayland-protocols/wayland_client.hpp TARGET ${target})

    # drm-lease-v1 — the wayland-leased-drm backend's entire Wayland surface
    # area: bind wp_drm_lease_device_v1, enumerate connectors, submit a lease
    # request, then monitor for finished/withdrawn. Vendored: the staging XML
    # only ships with wayland-protocols >= 1.22 and the project floor is 1.20
    # (see the >=1.20 pkg_check_modules above), so it cannot be taken from
    # IVI_WL_PROTOCOLS_BASE. No shipped wl/ helper carries its interface
    # tables, so generate self-contained with EMIT_INTERFACE_TABLES.
    if (BUILD_BACKEND_WAYLAND_LEASED_DRM)
        wayland_cxx_generate(PROTOCOL "${_loc}/drm-lease-v1.xml"
            MODE client-header EMIT_INTERFACE_TABLES
            OUTPUT wayland-protocols/drm_lease_v1_client.hpp TARGET ${target})
    endif ()

    if (NOT IVI_WAYLAND_SURFACE_BACKENDS)
        return()
    endif ()

    # presentation-time — every surface build (IVsyncProvider); no shipped header -> emit tables.
    wayland_cxx_generate(PROTOCOL "${_pres}" MODE client-header EMIT_INTERFACE_TABLES
        OUTPUT wayland-protocols/presentation_time_client.hpp TARGET ${target})

    # input-timestamps — every surface build (ivi::InputTimestamps: high-resolution kernel
    # timestamps for the pointer/touch → Flutter path; runtime-optional, the
    # provider no-ops when the compositor doesn't advertise the manager).
    # No shipped header -> emit tables.
    wayland_cxx_generate(PROTOCOL
        "${IVI_WL_PROTOCOLS_BASE}/unstable/input-timestamps/input-timestamps-unstable-v1.xml"
        MODE client-header EMIT_INTERFACE_TABLES
        OUTPUT wayland-protocols/input_timestamps_client.hpp TARGET ${target})

    # viewporter — every surface build (per-surface scaling; stable since wayland-protocols
    # 1.4, so present on Dunfell's 1.20). No shipped header -> emit tables.
    wayland_cxx_generate(PROTOCOL
        "${IVI_WL_PROTOCOLS_BASE}/stable/viewporter/viewporter.xml"
        MODE client-header EMIT_INTERFACE_TABLES
        OUTPUT wayland-protocols/viewporter_client.hpp TARGET ${target})

    # linux-dmabuf-unstable-v1 — the wayland_vulkan zero-copy present path binds
    # v4 feedback for the compositor's scanout modifiers. Shipped header
    # wl/linux_dmabuf.hpp carries the interface tables, so generate WITHOUT
    # --emit-interface-tables (same rule as xdg-shell). Generating it
    # unconditionally costs nothing on stacks that never bind it.
    wayland_cxx_generate(PROTOCOL
        "${IVI_WL_PROTOCOLS_BASE}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml"
        MODE client-header
        OUTPUT wayland-protocols/linux_dmabuf_client.hpp TARGET ${target})

    # fractional-scale-v1 — every surface build. Vendored: the XML only ships with
    # wayland-protocols >= 1.31, which Dunfell/Ubuntu-20.04 hosts predate.
    # Both protocols are gated at runtime on the compositor advertising the
    # global, so generating unconditionally costs nothing on old stacks.
    wayland_cxx_generate(PROTOCOL "${_loc}/fractional-scale-v1.xml"
        MODE client-header EMIT_INTERFACE_TABLES
        OUTPUT wayland-protocols/fractional_scale_v1_client.hpp TARGET ${target})

    # linux-drm-syncobj-v1 — the wayland_vulkan dma-buf present path binds this
    # for explicit sync (acquire/release timeline points that replace the CPU
    # fence). Vendored: the staging XML only ships with wayland-protocols >= 1.32,
    # newer than the oldest supported sysroots. No shipped wl/ helper carries its
    # interface tables, so generate self-contained with EMIT_INTERFACE_TABLES.
    # Runtime-gated on the compositor advertising the global, so generating it
    # unconditionally costs nothing on stacks that never bind it.
    wayland_cxx_generate(PROTOCOL "${_loc}/linux-drm-syncobj-v1.xml"
        MODE client-header EMIT_INTERFACE_TABLES
        OUTPUT wayland-protocols/linux_drm_syncobj_client.hpp TARGET ${target})

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
        # agl-shell — the shipped wl/agl_shell.hpp once hand-wrote the interface
        # table + wl_iface(), but wayland-cxx-scanner v1.0.0 dropped that (it
        # pinned the protocol version to whatever the table spelled out) and now
        # expects the table generated from the XML. So emit tables here, unlike
        # xdg/simple whose shipped headers still carry wl_iface() inline.
        wayland_cxx_generate(PROTOCOL "${_bund}/agl-shell.xml" MODE client-header
            EMIT_INTERFACE_TABLES
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
    if (TARGET PkgConfig::WAYLAND_CXX_SURFACE)
        target_link_libraries(${target} PRIVATE PkgConfig::WAYLAND_CXX_SURFACE)
    endif ()
    if (TARGET PkgConfig::WAYLAND_CXX_EGL)
        target_link_libraries(${target} PRIVATE PkgConfig::WAYLAND_CXX_EGL)
    endif ()
endfunction()
