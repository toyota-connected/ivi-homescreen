
#
# Flutter Common library
#
add_subdirectory(flutter)

#
# Filament
#
set(LINUX TRUE)

add_compile_definitions(FILAMENT_SUPPORTS_WAYLAND)

include_directories(.)

if (BUILD_BACKEND_WAYLAND_VULKAN)
    add_subdirectory(filament/libs/bluevk)
endif ()

#
# ASIO
#
add_subdirectory(asio-1-28-1)

#
# Speedlog
#
set(SPDLOG_NO_EXCEPTIONS ON)
set(SPDLOG_NO_THREAD_ID ON)
set(SPDLOG_BUILD_PIC ON)
set(SPDLOG_SANITIZE_ADDRESS ${SANITIZE_ADDRESS})
add_subdirectory(spdlog-1.12.0)

#
# Google Test
#
if (BUILD_UNIT_TESTS)
    add_subdirectory(googletest)
endif ()
