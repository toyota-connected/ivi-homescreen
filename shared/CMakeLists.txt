#
# Copyright 2026 Toyota Connected North America
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# ihs_shared: the C-ABI shared library that fronts logging, tracing,
# platform-view surface negotiation, and config read-back for out-of-tree
# Dart FFI plugins. See docs/PLUGIN_ABI.md for the boundary contract.
#
# Shared-only, on purpose: a static copy in both the shell and a plugin would
# give the process two copies of the logging bridge, ring registry, and trace
# sink table. There is exactly one copy of that state, in this .so.
#
# Buildable standalone (CI ABI gate + consumer smoke) without the homescreen
# tree: when this is the top-level project it establishes its own project() and
# an ENABLE_DLT option; otherwise it inherits both from the parent build.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(ihs_shared_standalone LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if (NOT DEFINED ENABLE_DLT)
        option(ENABLE_DLT "Build the DLT logging bridge into ihs_shared" ON)
    endif ()
endif ()

include(GNUInstallDirs)
include(GenerateExportHeader)
include(CMakePackageConfigHelpers)

set(IHS_SHARED_ABI_VERSION_STR 1.0.0)

add_library(ihs_shared SHARED
        src/ihs_api.cc
        src/trace.cc
        src/config.cc
        src/platform_view.cc
)
add_library(ivi_homescreen::ihs_shared ALIAS ihs_shared)

set_target_properties(ihs_shared PROPERTIES
        OUTPUT_NAME ihs_shared
        VERSION ${IHS_SHARED_ABI_VERSION_STR}
        SOVERSION 1
        CXX_VISIBILITY_PRESET hidden
        C_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
)

# Generates <build>/include/ihs/ihs_export.h defining IHS_EXPORT. With the
# hidden visibility presets above, only the entry points marked IHS_EXPORT are
# candidates for export; the version script below is the belt-and-suspenders
# that also strips anything that slips through.
generate_export_header(ihs_shared
        BASE_NAME IHS
        EXPORT_FILE_NAME ${CMAKE_CURRENT_BINARY_DIR}/include/ihs/ihs_export.h
)

target_include_directories(ihs_shared
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Restrict the dynamic symbol table to the ihs_* C ABI (see ihs_shared.map).
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(_ihs_version_script ${CMAKE_CURRENT_SOURCE_DIR}/ihs_shared.map)
    target_link_options(ihs_shared PRIVATE
            "LINKER:--version-script=${_ihs_version_script}")
    set_target_properties(ihs_shared PROPERTIES
            LINK_DEPENDS ${_ihs_version_script})
endif ()

# Match the shell's standard library selection (libc++ on clang builds) so the
# bridge compiled in here links cleanly against the shell.
if (TARGET toolchain)
    target_link_libraries(ihs_shared PRIVATE toolchain::toolchain)
endif ()
if (COMMAND ihs_apply_cxx_compat)
    ihs_apply_cxx_compat(ihs_shared)
endif ()

target_compile_options(ihs_shared PRIVATE -Wall -Wextra)

# Never unload this library: the DLT bridge owns a worker thread and per-thread
# ring TLS whose destructors must not fire against code unmapped by dlclose.
# -z nodelete pins it for the process lifetime even if a plugin that pulled it
# in is unloaded.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_options(ihs_shared PRIVATE "LINKER:-z,nodelete")
endif ()

# Logging capability. The generic logging surface (the ihs_log_* ABI, the ring
# registry / drain worker, and the console, file, and level-floor sinks) is
# always compiled in, so IhsApi::logging is always present. ENABLE_DLT gates
# only the DLT-specific pieces — the runtime libdlt loader and the DLT sink;
# with it off, "dlt" is simply not an available IHS_LOG_SINK and the surface
# falls back to console. The DLT sink dlopens libdlt at runtime, so even the
# ENABLE_DLT path adds no build-time dependency of its own.
find_package(Threads REQUIRED)
target_sources(ihs_shared PRIVATE
        src/logging/thread_ring.cpp
        src/logging/ring_registry.cpp
        src/logging/context_cache.cpp
        src/logging/worker.cpp
        src/logging/bridge.cpp
        src/logging/ffi_shim.cpp
        src/logging/console_sink.cpp
        src/logging/file_sink.cpp
        src/logging/sink_set.cpp
)
target_include_directories(ihs_shared PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/logging
)
target_link_libraries(ihs_shared PRIVATE Threads::Threads)

if (ENABLE_DLT)
    target_sources(ihs_shared PRIVATE
            src/logging/libdlt_loader.cpp
            src/logging/dlt_sink.cpp
    )
    target_compile_definitions(ihs_shared PRIVATE ENABLE_DLT=1)
    target_link_libraries(ihs_shared PRIVATE ${CMAKE_DL_LIBS})
endif ()

# Location service (the ihs_location_* C ABI): the gpsd and geoclue providers
# and their gpsd-primary/geoclue-fallback combiner. geoclue talks D-Bus through
# sd-bus, which it loads from libsystemd.so.0 at runtime via dlopen — exactly as
# the DLT sink loads libdlt — so ihs_shared keeps no build- or link-time
# dependency on libsystemd; only libdl (for dlopen) is needed.
target_sources(ihs_shared PRIVATE
        src/location/location_api.cc
        src/location/registry.cc
        src/location/manager.cc
        src/location/gpsd_provider.cc
        src/location/geoclue_provider.cc
        src/location/fallback_source.cc
        src/location/file_source.cc
        src/location/kalman_cv.cc
        src/location/sd_bus_dynamic.cc
)
target_link_libraries(ihs_shared PRIVATE ${CMAKE_DL_LIBS})

# Flutter desktop embedder C API forwarders (the FlutterDesktop* surface; see
# include/ihs/flutter_desktop_bridge.h). They forward to a proc table the shell
# installs at engine bring-up, so a plugin dlopen'd into the executable resolves
# the whole host ABI -- ihs_* and FlutterDesktop* alike -- from this one .so.
#
# Only meaningful inside the homescreen tree: the standalone ABI-gate build has
# no embedder to install the table, and the public embedder headers live under
# third_party/flutter, outside this directory. So the source is gated on those
# headers being present; the standalone build simply omits the forwarders (its
# ihs_* surface is unchanged). FLUTTER_DESKTOP_LIBRARY (defined in the source)
# makes the FLUTTER_EXPORT annotations resolve to default visibility despite the
# hidden preset; the version script then keeps them in the dynamic table.
set(_ihs_flutter_public
        "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/flutter/shell/platform/common/public")
if (EXISTS "${_ihs_flutter_public}/flutter_messenger.h" AND
        EXISTS "${_ihs_flutter_public}/flutter_plugin_registrar.h" AND
        EXISTS "${_ihs_flutter_public}/flutter_texture_registrar.h")
    target_sources(ihs_shared PRIVATE src/flutter_desktop_bridge.cc)
    # Only the forwarder source includes the embedder headers, but attach the
    # path at target scope: it applies consistently across generators and IDEs,
    # and no other source pulls those headers in.
    target_include_directories(ihs_shared PRIVATE "${_ihs_flutter_public}")
endif ()

#
# Install + CMake package export
#
install(TARGETS ihs_shared EXPORT ihs_shared_targets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Public headers (hand-written) + the generated export header.
#
# flutter_desktop_bridge.h is deliberately not installed. Its one exported
# symbol, ihs_flutter_desktop_set_procs, is a real ihs_* entry point (the shell
# calls it to install the proc table), but the header that declares it #includes
# the Flutter embedder headers, which ihs_shared does not ship -- so an installed
# copy could not compile standalone. It is a shell-internal integration point,
# consumed in-tree from the source include directory; nothing out-of-tree needs
# to declare the setter.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/ihs
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h"
        PATTERN "flutter_desktop_bridge.h" EXCLUDE
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/include/ihs/ihs_export.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ihs
)

install(EXPORT ihs_shared_targets
        FILE ivi-homescreen-shared-targets.cmake
        NAMESPACE ivi_homescreen::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ivi-homescreen-shared
)

configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ivi-homescreen-shared-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ivi-homescreen-shared-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ivi-homescreen-shared
)
write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ivi-homescreen-shared-config-version.cmake
        VERSION ${IHS_SHARED_ABI_VERSION_STR}
        COMPATIBILITY SameMajorVersion
)
install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ivi-homescreen-shared-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ivi-homescreen-shared-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ivi-homescreen-shared
)

# pkg-config for meson / Yocto / quick sanity checks.
set(PKGCONFIG_LIBDIR ${CMAKE_INSTALL_FULL_LIBDIR})
set(PKGCONFIG_INCLUDEDIR ${CMAKE_INSTALL_FULL_INCLUDEDIR})
set(IHS_SHARED_PC_VERSION ${IHS_SHARED_ABI_VERSION_STR})
configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ivi-homescreen-shared.pc.in
        ${CMAKE_CURRENT_BINARY_DIR}/ivi-homescreen-shared.pc @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ivi-homescreen-shared.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)
