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
if(COVERAGE)
    set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} --coverage")
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
    # The three input sources below are gated on BUILD_SOFTWARE_INPUT_LIBINPUT /
    # DRM backends in the shell target and are therefore absent from
    # TYPICAL_TEST_SOURCES in a default Wayland-EGL unit-test configure.
    # Include them unconditionally here so every test driver (wake_event_fd,
    # xkb_keyboard, key_repeater) can always link against the archive.
    ${PROJECT_SOURCE_DIR}/shell/input/wake_event_fd.cc
    ${PROJECT_SOURCE_DIR}/shell/input/xkb_keyboard.cc
    ${PROJECT_SOURCE_DIR}/shell/input/key_repeater.cc
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
    # Phase 2 test drivers that use GMock include gmock/gmock.h; propagate
    # the gmock include path so it is available without repeating it in each
    # per-test CMakeLists.txt.
    $<TARGET_PROPERTY:gmock,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(homescreen_ut_shell
    # PUBLIC: test executables that link this archive must also see the
    # transitive link deps (wayland, EGL, flutter, toolchain isystem paths).
    PUBLIC
    ${TYPICAL_TEST_LINK_LIBS}
    toolchain::toolchain
)


#add_subdirectory(template-test-case)
add_subdirectory(utils-test)
# flutter_view-test and app-test require a live Wayland compositor connection
# (Display::Display calls wl_display_connect / App::BuildDisplays).  They have
# been moved to test/integration/ and are not run by the unit-test-coverage job.
add_subdirectory(configuration-test-case)
add_subdirectory(output_resolver-test)
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
add_subdirectory(mutation_stack-test)
add_subdirectory(pixel_swizzle-test)
add_subdirectory(scale_policy-test)
add_subdirectory(text_input-test)
add_subdirectory(hexdump-test)
add_subdirectory(stats-test)
add_subdirectory(frame_profile-test)
add_subdirectory(motion_to_photon-test)
add_subdirectory(xkb_keyboard-test)
add_subdirectory(key_repeater-test)
add_subdirectory(shutdown_flag-test)
# ----- Phase 2: mock / stub tests -----------------------------------------
add_subdirectory(output_manager-test)
add_subdirectory(main_loop_waker-test)
add_subdirectory(vsync_provider-test)
add_subdirectory(logging_handler-test)
add_subdirectory(platform_handler-test)
add_subdirectory(mouse_cursor_handler-test)
add_subdirectory(watchdog_plugin-test)
add_subdirectory(key_event_handler-test)
add_subdirectory(key_mapping-test)
add_subdirectory(flutter_desktop_messenger-test)
add_subdirectory(drm_device_resolver-test)
# texture-test requires a live Wayland compositor, Flutter engine, and an app
# bundle on disk — it is an integration test and has been moved to
# test/integration/texture-test/.  Do NOT re-add it here.

# The accessibility sources are only part of the shell target (and therefore of
# TYPICAL_TEST_SOURCES) when BUILD_ACCESSIBILITY is configured on.
if (BUILD_ACCESSIBILITY)
    add_subdirectory(semantics_translator-test)
    add_subdirectory(accessibility_tree-test)
endif ()
