# Dear ImGui (vendored 1.92.8) + the render backends the shell's debug HUD needs.
# Core is always built; the Vulkan and/or OpenGL-ES render backends are compiled
# only for the ivi backends that are enabled, so a GL-only or Vulkan-only build
# does not drag in the other's headers.
add_library(imgui STATIC
        imgui.cpp
        imgui_draw.cpp
        imgui_tables.cpp
        imgui_widgets.cpp
)

target_include_directories(imgui PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/backends
)

# Vulkan render backend. Loader-based (IMGUI_IMPL_VULKAN_NO_PROTOTYPES): it calls
# no Vulkan symbols directly, so the shell feeds it the SAME interposed loader it
# hands plugins (queue-locking), rather than linking libvulkan.
if (BUILD_BACKEND_WAYLAND_VULKAN OR BUILD_BACKEND_DRM_KMS_VULKAN)
    target_sources(imgui PRIVATE backends/imgui_impl_vulkan.cpp)
    target_compile_definitions(imgui PUBLIC
            IMGUI_IMPL_VULKAN_NO_PROTOTYPES
            VK_NO_PROTOTYPES
    )
    target_link_libraries(imgui PUBLIC vulkan_headers)
endif ()

# OpenGL-ES render backend for the EGL backends. imgui_impl_opengl3 resolves GL
# entry points through its own bundled loader (imgui_impl_opengl3_loader.h), so
# no external GL loader is linked; IMGUI_IMPL_OPENGL_ES2 selects the GLES 2 path
# (GLSL "#version 100"), matching the EGL backends' GLES2 context.
if (BUILD_BACKEND_WAYLAND_EGL OR BUILD_BACKEND_DRM_KMS_EGL)
    target_sources(imgui PRIVATE backends/imgui_impl_opengl3.cpp)
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_ES2)
endif ()

# Vendored third-party: don't fail our -Werror-ish flags on its code.
target_compile_options(imgui PRIVATE -w)
