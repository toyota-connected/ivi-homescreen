
add_library(platform_homescreen STATIC
        flutter_desktop.cc
        flutter_desktop_messenger.cc
        platform_handler.cc
        platform_views/platform_views_handler.cc
        platform_views/platform_views_registry.cc
        platform_views/platform_view_touch.cc
)

target_include_directories(platform_homescreen PUBLIC
        .
        ./platform_views/layer_playground
        client_wrapper/include
        public
        ${CMAKE_BINARY_DIR}/shell
        ${CMAKE_SOURCE_DIR}/shell
)

target_include_directories(platform_homescreen PRIVATE
        ../..
        ../../..
        ../../logging
        ../../../plugins
        platform_views
)

target_compile_definitions(platform_homescreen PUBLIC
        PATH_PREFIX="${CMAKE_INSTALL_PREFIX}"
)

target_link_libraries(platform_homescreen PUBLIC
        asio
        spdlog
        wayland-gen
        flutter_common
)

if (BUILD_VIEW_LAYER_PLAYGROUND)
    target_compile_definitions(platform_homescreen PUBLIC ENABLE_VIEW_LAYER_PLAYGROUND)
    target_sources(platform_homescreen PRIVATE
            platform_views/layer_playground/layer_playground.cc
    )
endif ()

if (BUILD_VIEW_FILAMENT_VIEW)
    target_compile_definitions(platform_homescreen PUBLIC ENABLE_VIEW_FILAMENT_VIEW)
    include(filament)
    target_sources(platform_homescreen PRIVATE
            platform_views/filament_view/filament_view.cc
            platform_views/filament_view/models/model/animation.cc
            platform_views/filament_view/models/model/animation_manager.cc
            platform_views/filament_view/models/model/glb/loader/glb_loader.cc
            platform_views/filament_view/models/model/gltf/loader/gltf_loader.cc
            platform_views/filament_view/models/model/model.cc
            platform_views/filament_view/models/scene/camera/camera.cc
            platform_views/filament_view/models/scene/camera/camera_manager.cc
            platform_views/filament_view/models/scene/camera/exposure.cc
            platform_views/filament_view/models/scene/camera/lens_projection.cc
            platform_views/filament_view/models/scene/camera/projection.cc
            platform_views/filament_view/models/scene/geometry/direction.cc
            platform_views/filament_view/models/scene/geometry/position.cc
            platform_views/filament_view/models/scene/geometry/size.cc
            platform_views/filament_view/models/scene/indirect_light/indirect_light.cc
            platform_views/filament_view/models/scene/indirect_light/indirect_light_manager.cc
            platform_views/filament_view/models/scene/light/light.cc
            platform_views/filament_view/models/scene/light/light_manager.cc
            platform_views/filament_view/models/scene/material/material.cc
            platform_views/filament_view/models/scene/material/material_manager.cc
            platform_views/filament_view/models/scene/material/material_parameter.cc
            platform_views/filament_view/models/scene/material/texture/texture.cc
            platform_views/filament_view/models/scene/material/texture/texture_sampler.cc
            platform_views/filament_view/models/scene/skybox/skybox.cc
            platform_views/filament_view/models/scene/skybox/skybox_manager.cc
            platform_views/filament_view/models/scene/ground.cc
            platform_views/filament_view/models/scene/ground_manager.cc
            platform_views/filament_view/models/scene/scene.cc
            platform_views/filament_view/models/shapes/shape.cc
            platform_views/filament_view/models/shapes/shape_manager.cc
            platform_views/filament_view/viewer/custom_model_viewer.cc
    )
    target_link_libraries(platform_homescreen PUBLIC
            filament
    )
endif ()
