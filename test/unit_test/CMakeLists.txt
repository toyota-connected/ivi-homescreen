# TYPICAL_TEST_SOURCES      : all *.cc files of typical test
# TYPICAL_TEST_DEFINITIONS  : compile definition of typical test
# TYPICAL_TEST_INC_DIRS     : include directories of typical test
# TYPICAL_TEST_LINK_LIBS    : link libraries of typical test
get_target_property(TYPICAL_TEST_SOURCES ${PROJECT_NAME} SOURCES)
list(FILTER TYPICAL_TEST_SOURCES EXCLUDE REGEX ".*main\.cc")
list(TRANSFORM TYPICAL_TEST_SOURCES PREPEND ${PROJECT_SOURCE_DIR}/shell/)
list(APPEND TYPICAL_TEST_SOURCES ${PROJECT_SOURCE_DIR}/test/unit_test/unit_test_utils/unit_test_utils.cc)

# If TYPICAL_TEST_DEFINITIONS is set to the same as the homescreen build, it will change depending on the cmake arguments.
# When tweaking TYPICAL_TEST_DEFINITIONS in CMakeLists.txt for each test case, it is better to always set TYPICAL_TEST_DEFINITIONS the same.
# Therefore, TYPICAL_TEST_DEFINITIONS is set one by one without including options.cmake and plugins.cmake.
list(APPEND TYPICAL_TEST_DEFINITIONS ENABLE_XDG_CLIENT)
list(APPEND TYPICAL_TEST_DEFINITIONS ENABLE_EGL)
list(APPEND TYPICAL_TEST_DEFINITIONS BUILD_EXAMPLES)
list(APPEND TYPICAL_TEST_DEFINITIONS HAVE_STRCHRNUL)
list(APPEND TYPICAL_TEST_DEFINITIONS EGL_NO_X11)
list(APPEND TYPICAL_TEST_DEFINITIONS MESA_EGL_NO_X11_HEADERS)
list(APPEND TYPICAL_TEST_DEFINITIONS HAVE_MEMFD_CREATE)
list(APPEND TYPICAL_TEST_DEFINITIONS PATH_PREFIX="${CMAKE_INSTALL_PREFIX}")
list(APPEND TYPICAL_TEST_DEFINITIONS EXE_OUTPUT_NAME="${EXE_OUTPUT_NAME}")
list(APPEND TYPICAL_TEST_DEFINITIONS WL_EGL_PLATFORM)
list(APPEND TYPICAL_TEST_DEFINITIONS FILAMENT_SUPPORTS_WAYLAND)
list(APPEND TYPICAL_TEST_DEFINITIONS ASIO_STANDALONE)
list(APPEND TYPICAL_TEST_DEFINITIONS ASIO_HEADER_ONLY)
list(APPEND TYPICAL_TEST_DEFINITIONS ASIO_NO_TYPEID)
list(APPEND TYPICAL_TEST_DEFINITIONS ASIO_DISABLE_HANDLER_TYPE_REQUIREMENTS)
list(APPEND TYPICAL_TEST_DEFINITIONS ASIO_NO_DEPRECATED)

list(APPEND TYPICAL_TEST_DEFINITIONS UNIT_TEST)

if (BUILD_COMPOSITOR)
    list(APPEND TYPICAL_TEST_DEFINITIONS BUILD_COMPOSITOR=1)
else ()
    list(APPEND TYPICAL_TEST_DEFINITIONS BUILD_COMPOSITOR=0)
endif ()

list(APPEND TYPICAL_TEST_DEFINITIONS TEST_APP_BUNDLE_PATH="${UNIT_TEST_APP_BUNDLE}")
list(APPEND TYPICAL_TEST_DEFINITIONS GOLDEN_IMAGE_PATH="${CMAKE_BINARY_DIR}/test/unit_test/test_images_golden/")
list(APPEND TYPICAL_TEST_DEFINITIONS TEST_IMAGE_PATH="${CMAKE_BINARY_DIR}/test/unit_test/test_images/")

include (TestBigEndian)
TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
list(APPEND TYPICAL_TEST_DEFINITIONS IS_ARCH_BIG_ENDIAN=${IS_BIG_ENDIAN})

# Match the shell target: when the Wayland-Vulkan backend is built (and so may
# coexist with DRM-KMS-Vulkan), configure <vulkan/vulkan.h> with the Wayland WSI
# so the shell sources reused here as TYPICAL_TEST_SOURCES (register_backends.cc
# / flutter_view.cc reach both Vulkan backend headers) see
# VkWaylandSurfaceCreateInfoKHR. Mirrors shell/CMakeLists.txt.
if (BUILD_BACKEND_WAYLAND_VULKAN)
    list(APPEND TYPICAL_TEST_DEFINITIONS VK_USE_PLATFORM_WAYLAND_KHR=1)
endif ()

get_target_property(TYPICAL_TEST_INC_DIRS ${PROJECT_NAME} INCLUDE_DIRECTORIES)
list(APPEND TYPICAL_TEST_INC_DIRS ${CMAKE_BINARY_DIR}/shell)
list(APPEND TYPICAL_TEST_INC_DIRS ${PROJECT_SOURCE_DIR}/test/unit_test/unit_test_utils)
get_target_property(TYPICAL_TEST_LINK_LIBS ${PROJECT_NAME} LINK_LIBRARIES)
set(CMAKE_THREAD_PREFER_PTHREAD ON)
include(FindThreads)

# Calculate test coverage
#
# -fprofile-update=atomic is required, not an optimization: gcov's counters are
# plain increments by default, so two threads executing the same instrumented
# line race and can leave a counter negative. geninfo then refuses the whole
# capture ("Unexpected negative count"), failing the coverage job rather than
# just skewing a number. Any test that runs instrumented code on more than one
# thread hits this. The atomic updates cost throughput, which the coverage
# build can afford and the other builds never see.
if(COVERAGE)
    set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} --coverage -fprofile-update=atomic")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/run-ut-coverage.sh
        DESTINATION ${CMAKE_BINARY_DIR})
endif()

# Shared OBJECT library: compile all shell + unit_test_utils sources ONCE
# and let every test driver link against it. Avoids the O(N_tests × M_sources)
# recompilation that the old pattern caused — 22 tests × ~25 shell TUs was
# ~550 redundant compilation units (very visible in CI with -DCOVERAGE=ON).
#
# OBJECT (not STATIC) is intentional: CMake expands an OBJECT library into
# a flat list of .o files in each consumer's link command, identical to how
# ${TYPICAL_TEST_SOURCES} behaved when compiled directly into each executable.
# This eliminates the circular-dependency link-ordering issue that a STATIC
# archive has between libflutter.a and libplatform_homescreen.a: with a flat
# .o list the GNU linker resolves back-references on its normal forward pass.
#
# Coverage: all test drivers write gcda hits into the shared .o files' gcda
# paths (sequential accumulation). Do NOT use ctest -j > 1 with COVERAGE=ON.
add_library(homescreen_ut_shell OBJECT
    ${TYPICAL_TEST_SOURCES}
    ${PROJECT_SOURCE_DIR}/shell/input/wake_event_fd.cc
)
target_compile_definitions(homescreen_ut_shell
    # PUBLIC: test executables inherit the compile-time defines (UNIT_TEST,
    # TEST_APP_BUNDLE_PATH, GOLDEN_IMAGE_PATH, ASIO_* flags, etc.) so they
    # do not need to repeat them in each per-test CMakeLists.
    PUBLIC
    ${TYPICAL_TEST_DEFINITIONS}
)
target_include_directories(homescreen_ut_shell
    # PUBLIC so that every test driver that links against the archive
    # automatically inherits the shell and unit_test_utils include paths,
    # without each per-test CMakeLists needing to repeat them.
    PUBLIC
    ${TYPICAL_TEST_INC_DIRS}
    # unit_test_utils.cc (in the archive) and test case .cc files both
    # include gtest/gtest.h; propagate it through the archive.
    $<TARGET_PROPERTY:gtest,INTERFACE_INCLUDE_DIRECTORIES>
    # platform_homescreen is linked PRIVATE below (to keep its INTERFACE_SOURCES
    # from being compiled into every driver), which also drops its PUBLIC
    # include-dir usage requirements. Some shell headers reused as
    # TYPICAL_TEST_SOURCES (flutter_view.h / engine.h) and test cases
    # (test_case_text_input.cc) include platform_homescreen's public headers
    # (flutter_desktop_view_controller_state.h, flutter_desktop_engine_state.h),
    # so re-propagate just its include dirs here.
    $<TARGET_PROPERTY:platform_homescreen,INTERFACE_INCLUDE_DIRECTORIES>
)
# platform_homescreen is a STATIC library that re-exports its platform-view
# sources (platform_views_handler.cc / platform_view_registry.cc /
# platform_view_touch.cc) as INTERFACE_SOURCES via target_sources(... PUBLIC ...)
# — so every target that links it *compiles* those .cc into itself. That is fine
# for the shell binary (a static archive only pulls .o on demand), but this
# OBJECT library's object files are always linked, and linking platform_homescreen
# PUBLIC would re-propagate its INTERFACE_SOURCES to every test driver as well.
# The driver would then compile the same three TUs a second time, producing
# "multiple definition of PlatformViewsHandler/PlatformViewRegistry/PlatformViewTouch"
# at link time. So split platform_homescreen out of the PUBLIC link set and link
# it PRIVATE: its interface sources are then compiled exactly once — here, into
# homescreen_ut_shell — while its archive (and usage requirements) still
# propagate to the drivers that link this object library. Keep everything else
# PUBLIC so drivers inherit the transitive link deps (wayland, EGL, flutter,
# toolchain isystem paths).
#
# Use a LOCAL copy for the split: the old-style test subdirectories added below
# link ${TYPICAL_TEST_LINK_LIBS} directly and rely on platform_homescreen's
# PUBLIC include dirs (flutter_desktop_*.h), so the shared variable must stay
# intact.
set(HOMESCREEN_UT_SHELL_PUBLIC_LIBS ${TYPICAL_TEST_LINK_LIBS})
list(REMOVE_ITEM HOMESCREEN_UT_SHELL_PUBLIC_LIBS platform_homescreen)
target_link_libraries(homescreen_ut_shell
    PUBLIC
    ${HOMESCREEN_UT_SHELL_PUBLIC_LIBS}
    toolchain::toolchain
    PRIVATE
    platform_homescreen
)


#add_subdirectory(template-test-case)
add_subdirectory(utils-test)
# flutter_view-test and app-test require a live Wayland compositor connection
# (Display::Display calls wl_display_connect / App::BuildDisplays).  They have
# been moved to test/integration/ and are not run by the unit-test-coverage job.
add_subdirectory(configuration-test-case)
if (ENABLE_OSGI)
    add_subdirectory(osgi_config-test)
    add_subdirectory(osgi_bridge-test)
    add_subdirectory(osgi_vsync-test)
endif ()
add_subdirectory(output_resolver-test)
add_subdirectory(ihs_pv-test)
# T1 — drives the real LeaseClient against a mock drm-lease-v1 compositor.
# Needs no GPU/compositor, so it runs anywhere the lease client is built.
if (BUILD_BACKEND_WAYLAND_LEASED_DRM)
    add_subdirectory(lease_client-test)
    # T2 — kernel-real lease on vkms. Skips itself without vkms + DRM master, so
    # it is safe to build and run everywhere; it only does real work under sudo.
    #
    # Needs both halves of the leased scanout path it drives: the software sink
    # (BUILD_SOFTWARE_SINK_DRM) and the output provider, whose drm-cxx only
    # exists once a DRM-KMS backend pulls in cmake/drm_kms.cmake. Configure e.g.
    #   -DBUILD_BACKEND_WAYLAND_LEASED_DRM=ON -DBUILD_BACKEND_SOFTWARE=ON \
    #   -DBUILD_BACKEND_DRM_KMS_EGL=ON -DBUILD_UNIT_TESTS=ON
    if (BUILD_SOFTWARE_SINK_DRM AND TARGET drm-cxx)
        add_subdirectory(leased_drm_vkms-test)
    endif ()
endif ()
add_subdirectory(timer-test)
add_subdirectory(watchdog-test)
add_subdirectory(wake_event_fd-test)
add_subdirectory(libinput_log-test)
add_subdirectory(partial_repaint_gate-test)
add_subdirectory(gl_process_resolver-test)
add_subdirectory(shared_library-test)
add_subdirectory(backing_store_pool-test)
add_subdirectory(present_layer_sequencer-test)
add_subdirectory(compositor_registry-test)
add_subdirectory(platform_view_registry-test)
add_subdirectory(backend_output_binding-test)
add_subdirectory(vsync_park-test)
add_subdirectory(mutation_stack-test)
# The MCP registry is only compiled into ihs_shared when BUILD_MCP is on, so
# its tests would have nothing to link against otherwise.
if (BUILD_MCP)
    add_subdirectory(mcp_provider-test)
endif ()
add_subdirectory(pixel_swizzle-test)
add_subdirectory(scale_policy-test)
add_subdirectory(text_input-test)
# texture-test requires a live Wayland compositor, Flutter engine, and an app
# bundle on disk — it is an integration test and has been moved to
# test/integration/texture-test/.  Do NOT re-add it here.

# The accessibility sources are only part of the shell target (and therefore of
# TYPICAL_TEST_SOURCES) when BUILD_ACCESSIBILITY is configured on.
if (BUILD_ACCESSIBILITY)
    add_subdirectory(semantics_translator-test)
    add_subdirectory(accessibility_tree-test)
    add_subdirectory(semantics_publisher-test)
    # The hub itself is only in ihs_shared when BUILD_ACCESSIBILITY is on.
    add_subdirectory(semantics_hub-test)
endif ()
