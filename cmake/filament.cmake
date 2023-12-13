include_guard()

if (EXISTS ${FILAMENT_SRC_DIR})
    set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)

    add_library(filament INTERFACE)
    target_include_directories(filament INTERFACE
            ${FILAMENT_SRC_DIR}/filament/include
            ${FILAMENT_SRC_DIR}/filament/backend/include
            ${FILAMENT_SRC_DIR}/libs/filabridge/include
            ${FILAMENT_SRC_DIR}/libs/filamat/include
            ${FILAMENT_SRC_DIR}/libs/ibl/include
            ${FILAMENT_SRC_DIR}/libs/math/include
            ${FILAMENT_SRC_DIR}/libs/utils/include
            ${FILAMENT_SRC_DIR}/libs/gltfio/include
            ${FILAMENT_SRC_DIR}/libs
    )
    target_link_libraries(filament INTERFACE
            ${FILAMENT_BINARY_DIR}/libs/bluevk/libbluevk.a
            ${FILAMENT_BINARY_DIR}/libs/bluegl/libbluegl.a
            ${FILAMENT_BINARY_DIR}/filament/backend/libbackend.a
            ${FILAMENT_BINARY_DIR}/filament/backend/libvkshaders.a
            ${FILAMENT_BINARY_DIR}/third_party/smol-v/tnt/libsmol-v.a
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
            ${FILAMENT_BINARY_DIR}/filament/libfilament.a
            ${FILAMENT_BINARY_DIR}/third_party/meshoptimizer/tnt/libmeshoptimizer.a
            ${FILAMENT_BINARY_DIR}/third_party/mikktspace/libmikktspace.a
            ${FILAMENT_BINARY_DIR}/third_party/basisu/tnt/libzstd.a
            ${FILAMENT_BINARY_DIR}/third_party/draco/tnt/libdracodec.a
            ${FILAMENT_BINARY_DIR}/third_party/meshoptimizer/tnt/libmeshoptimizer.a
            ${FILAMENT_BINARY_DIR}/third_party/glslang/tnt/glslang/libglslang.a
            ${FILAMENT_BINARY_DIR}/third_party/glslang/tnt/SPIRV/libSPIRV.a
            ${FILAMENT_BINARY_DIR}/tools/matc/libmatlang.a

            #${FILAMENT_BINARY_DIR}/libs/matdbg/libmatdbg.a
            Threads::Threads
    )
endif ()
