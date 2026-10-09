set(TESTCASE_NAME "vkExternalTextureTest")

# Exercises the VkImage external-texture path (flutter/flutter#188855)
# without a GPU: resolve, validation, release tokens and deferred
# unregistration only move handles around. Builds just that TU rather than
# the full embedder set, so it needs neither GL nor a Vulkan loader.
add_executable(${TESTCASE_NAME}
        test_case_vk_external_texture.cc
        ${PROJECT_SOURCE_DIR}/shell/platform/homescreen/flutter_desktop_vk_texture.cc
)

add_sanitizers(${TESTCASE_NAME})

target_include_directories(${TESTCASE_NAME}
        PRIVATE
        ${PROJECT_SOURCE_DIR}/shell/platform/homescreen
        ${PROJECT_SOURCE_DIR}/third_party/flutter
        ${PROJECT_SOURCE_DIR}/third_party/flutter/shell/platform/common/public
)

target_link_libraries(${TESTCASE_NAME}
        PRIVATE
        gtest_main
        toolchain::toolchain
        vulkan_headers
)

add_test(
        NAME ${TESTCASE_NAME}
        COMMAND ${TESTCASE_NAME}
)
