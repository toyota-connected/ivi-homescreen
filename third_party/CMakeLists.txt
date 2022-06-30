
set(LINUX TRUE)

add_compile_definitions(FILAMENT_SUPPORTS_WAYLAND)

include_directories(.)

add_subdirectory(filament/libs/bluevk)