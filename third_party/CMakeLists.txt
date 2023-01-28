
set(LINUX TRUE)

add_compile_definitions(FILAMENT_SUPPORTS_WAYLAND)

include_directories(.)

if (BUILD_BACKEND_WAYLAND_VULKAN)
    add_subdirectory(filament/libs/bluevk)
endif  ()