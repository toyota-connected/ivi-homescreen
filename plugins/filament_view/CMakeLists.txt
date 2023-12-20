include_guard()

if (NOT EXISTS ${FILAMENT_SRC_DIR})
    message(FATAL_ERROR "${FILAMENT_SRC_DIR} does not exist")
endif ()
if (NOT EXISTS ${FILAMENT_BINARY_DIR})
    message(FATAL_ERROR "${FILAMENT_BINARY_DIR} does not exist")
endif ()

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

add_library(filament INTERFACE)
target_include_directories(filament INTERFACE
        ${FILAMENT_SRC_DIR}/filament/include
        ${FILAMENT_SRC_DIR}/filament/backend/include
        ${FILAMENT_SRC_DIR}/libs/camutils/include
        ${FILAMENT_SRC_DIR}/libs/filabridge/include
        ${FILAMENT_SRC_DIR}/libs/filamat/include
        ${FILAMENT_SRC_DIR}/libs/ibl/include
        ${FILAMENT_SRC_DIR}/libs/math/include
        ${FILAMENT_SRC_DIR}/libs/utils/include
        ${FILAMENT_SRC_DIR}/libs/gltfio/include
        ${FILAMENT_SRC_DIR}/libs

        ${FILAMENT_BINARY_DIR}/libs/gltfio
        ${FILAMENT_BINARY_DIR}/samples
)
target_link_libraries(filament INTERFACE
        ${FILAMENT_BINARY_DIR}/libs/bluevk/libbluevk.a
        ${FILAMENT_BINARY_DIR}/libs/bluegl/libbluegl.a
        ${FILAMENT_BINARY_DIR}/filament/backend/libbackend.a
        ${FILAMENT_BINARY_DIR}/filament/backend/libvkshaders.a
        ${FILAMENT_BINARY_DIR}/third_party/smol-v/tnt/libsmol-v.a
        ${FILAMENT_BINARY_DIR}/libs/camutils/libcamutils.a
        ${FILAMENT_BINARY_DIR}/libs/filamat/libfilamat.a
        ${FILAMENT_BINARY_DIR}/libs/filaflat/libfilaflat.a
        ${FILAMENT_BINARY_DIR}/libs/filabridge/libfilabridge.a
        ${FILAMENT_BINARY_DIR}/libs/geometry/libgeometry.a
        ${FILAMENT_BINARY_DIR}/libs/gltfio/libgltfio.a
        ${FILAMENT_BINARY_DIR}/libs/gltfio/libgltfio_core.a
        ${FILAMENT_BINARY_DIR}/libs/gltfio/libuberarchive.a
        ${FILAMENT_BINARY_DIR}/libs/ibl/libibl.a
        ${FILAMENT_BINARY_DIR}/libs/ktxreader/libktxreader.a
        ${FILAMENT_BINARY_DIR}/libs/matdbg/libmatdbg_combined.a
        ${FILAMENT_BINARY_DIR}/libs/utils/libutils.a
        ${FILAMENT_BINARY_DIR}/libs/uberz/libuberzlib.a
        ${FILAMENT_BINARY_DIR}/libs/viewer/libviewer.a
        ${FILAMENT_BINARY_DIR}/filament/libfilament.a
        ${FILAMENT_BINARY_DIR}/third_party/meshoptimizer/tnt/libmeshoptimizer.a
        ${FILAMENT_BINARY_DIR}/third_party/mikktspace/libmikktspace.a
        ${FILAMENT_BINARY_DIR}/third_party/basisu/tnt/libzstd.a
        ${FILAMENT_BINARY_DIR}/third_party/draco/tnt/libdracodec.a
        ${FILAMENT_BINARY_DIR}/third_party/meshoptimizer/tnt/libmeshoptimizer.a
        ${FILAMENT_BINARY_DIR}/third_party/glslang/tnt/glslang/libglslang.a
        ${FILAMENT_BINARY_DIR}/third_party/glslang/tnt/SPIRV/libSPIRV.a
        ${FILAMENT_BINARY_DIR}/tools/matc/libmatlang.a

        #${FILAMENT_BINARY_DIR}/samples/libsample-resources.a
        ${FILAMENT_BINARY_DIR}/samples/libgltf-demo-resources.a

        #${FILAMENT_BINARY_DIR}/libs/matdbg/libmatdbg.a
        Threads::Threads
)

add_library(plugin_filament_view STATIC
        filament_view_plugin_c_api.cc
        filament_view_plugin.cc
        messages.g.cc

        filament_scene.cc
        core/scene/scene_controller.cc

        core/model/animation/animation.cc
        core/model/animation/animation_manager.cc
        core/model/model_loader.cc
        core/model/model.cc
        core/scene/camera/camera.cc
        core/scene/camera/camera_manager.cc
        core/scene/camera/exposure.cc
        core/scene/camera/lens_projection.cc
        core/scene/camera/projection.cc
        core/scene/geometry/direction.cc
        core/scene/geometry/position.cc
        core/scene/geometry/size.cc
        core/scene/ground.cc
        core/scene/ground_manager.cc
        core/scene/indirect_light/indirect_light.cc
        core/scene/indirect_light/indirect_light_manager.cc
        core/scene/light/light.cc
        core/scene/light/light_manager.cc
        core/scene/material/material.cc
        core/scene/material/material_manager.cc
        core/scene/material/material_parameter.cc
        core/scene/material/texture/texture.cc
        core/scene/material/texture/texture_sampler.cc
        core/scene/skybox/skybox.cc
        core/scene/skybox/skybox_manager.cc
        core/scene/scene.cc
        core/shapes/shape.cc
        core/shapes/shape_manager.cc
        viewer/custom_model_viewer.cc
)

target_compile_definitions(plugin_filament_view PUBLIC ENABLE_PLUGIN_FILAMENT_VIEW)

target_include_directories(plugin_filament_view PUBLIC
        .
        include
)

# target_link_directories(plugin_filament_view PUBLIC ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(plugin_filament_view PUBLIC
        asio
        filament
        flutter_common
        platform_homescreen
)

add_sanitizers(plugin_filament_view)

if (IPO_SUPPORT_RESULT)
    set_property(TARGET plugin_filament_view PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif ()
