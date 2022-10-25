/*
 * Copyright 2022 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include "../include/comp_surf_vulkan/comp_surf_vulkan.h"
#include "VkBootstrap.h"

#include <dlfcn.h>
#include <memory>
#include <string>

#include <vulkan/vulkan_wayland.h>

struct CompSurfContext {
public:
    static uint32_t version();

    CompSurfContext(const char *accessToken,
                    int width,
                    int height,
                    void *nativeWindow,
                    const char *assetsPath);

    ~CompSurfContext();

    CompSurfContext(const CompSurfContext &) = delete;

    CompSurfContext(CompSurfContext &&) = delete;

    CompSurfContext &operator=(const CompSurfContext &) = delete;

    void render(double time);

    void resize(int width, int height);

private:
    static constexpr int kMaxFramesInFlight = 2;

    struct VulkanLibrary {
        void *library;

        VulkanLibrary() {
            library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
            if (!library)
                library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
            if (!library)
                return;
            vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                    dlsym(library, "vkGetInstanceProcAddr"));
        }

        void close() {
            dlclose(library);
            library = 0;
        }

        void init(VkInstance instance) {
            vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) vkGetInstanceProcAddr(
                    instance, "vkGetDeviceProcAddr");
            vkDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR) vkGetInstanceProcAddr(
                    instance, "vkDestroySurfaceKHR");
            vkCreateWaylandSurfaceKHR =
                    (PFN_vkCreateWaylandSurfaceKHR) vkGetInstanceProcAddr(
                            instance, "vkCreateWaylandSurfaceKHR");
        }

        void init(VkDevice device) {
            vkCreateRenderPass = (PFN_vkCreateRenderPass) vkGetDeviceProcAddr(
                    device, "vkCreateRenderPass");
            vkCreateShaderModule = (PFN_vkCreateShaderModule) vkGetDeviceProcAddr(
                    device, "vkCreateShaderModule");
            vkCreatePipelineLayout = (PFN_vkCreatePipelineLayout) vkGetDeviceProcAddr(
                    device, "vkCreatePipelineLayout");
            vkCreateGraphicsPipelines =
                    (PFN_vkCreateGraphicsPipelines) vkGetDeviceProcAddr(
                            device, "vkCreateGraphicsPipelines");
            vkDestroyShaderModule = (PFN_vkDestroyShaderModule) vkGetDeviceProcAddr(
                    device, "vkDestroyShaderModule");
            vkCreateFramebuffer = (PFN_vkCreateFramebuffer) vkGetDeviceProcAddr(
                    device, "vkCreateFramebuffer");
            vkCreateCommandPool = (PFN_vkCreateCommandPool) vkGetDeviceProcAddr(
                    device, "vkCreateCommandPool");
            vkAllocateCommandBuffers =
                    (PFN_vkAllocateCommandBuffers) vkGetDeviceProcAddr(
                            device, "vkAllocateCommandBuffers");
            vkBeginCommandBuffer = (PFN_vkBeginCommandBuffer) vkGetDeviceProcAddr(
                    device, "vkBeginCommandBuffer");
            vkEndCommandBuffer = (PFN_vkEndCommandBuffer) vkGetDeviceProcAddr(
                    device, "vkEndCommandBuffer");
            vkCmdSetViewport =
                    (PFN_vkCmdSetViewport) vkGetDeviceProcAddr(device, "vkCmdSetViewport");
            vkCmdSetScissor =
                    (PFN_vkCmdSetScissor) vkGetDeviceProcAddr(device, "vkCmdSetScissor");
            vkCmdBeginRenderPass = (PFN_vkCmdBeginRenderPass) vkGetDeviceProcAddr(
                    device, "vkCmdBeginRenderPass");
            vkCmdEndRenderPass = (PFN_vkCmdEndRenderPass) vkGetDeviceProcAddr(
                    device, "vkCmdEndRenderPass");
            vkCmdBindPipeline = (PFN_vkCmdBindPipeline) vkGetDeviceProcAddr(
                    device, "vkCmdBindPipeline");
            vkCmdDraw = (PFN_vkCmdDraw) vkGetDeviceProcAddr(device, "vkCmdDraw");
            vkCreateSemaphore = (PFN_vkCreateSemaphore) vkGetDeviceProcAddr(
                    device, "vkCreateSemaphore");
            vkCreateFence =
                    (PFN_vkCreateFence) vkGetDeviceProcAddr(device, "vkCreateFence");
            vkDeviceWaitIdle =
                    (PFN_vkDeviceWaitIdle) vkGetDeviceProcAddr(device, "vkDeviceWaitIdle");
            vkDestroyCommandPool = (PFN_vkDestroyCommandPool) vkGetDeviceProcAddr(
                    device, "vkDestroyCommandPool");
            vkDestroyFramebuffer = (PFN_vkDestroyFramebuffer) vkGetDeviceProcAddr(
                    device, "vkDestroyFramebuffer");
            vkWaitForFences =
                    (PFN_vkWaitForFences) vkGetDeviceProcAddr(device, "vkWaitForFences");
            vkAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR) vkGetDeviceProcAddr(
                    device, "vkAcquireNextImageKHR");
            vkResetFences =
                    (PFN_vkResetFences) vkGetDeviceProcAddr(device, "vkResetFences");
            vkQueueSubmit =
                    (PFN_vkQueueSubmit) vkGetDeviceProcAddr(device, "vkQueueSubmit");
            vkQueuePresentKHR = (PFN_vkQueuePresentKHR) vkGetDeviceProcAddr(
                    device, "vkQueuePresentKHR");
            vkDestroySemaphore = (PFN_vkDestroySemaphore) vkGetDeviceProcAddr(
                    device, "vkDestroySemaphore");
            vkDestroyFence =
                    (PFN_vkDestroyFence) vkGetDeviceProcAddr(device, "vkDestroyFence");
            vkDestroyPipeline = (PFN_vkDestroyPipeline) vkGetDeviceProcAddr(
                    device, "vkDestroyPipeline");
            vkDestroyPipelineLayout =
                    (PFN_vkDestroyPipelineLayout) vkGetDeviceProcAddr(
                            device, "vkDestroyPipelineLayout");
            vkDestroyRenderPass = (PFN_vkDestroyRenderPass) vkGetDeviceProcAddr(
                    device, "vkDestroyRenderPass");
        }

        PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = VK_NULL_HANDLE;
        PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = VK_NULL_HANDLE;

        PFN_vkCreateRenderPass vkCreateRenderPass = VK_NULL_HANDLE;
        PFN_vkCreateShaderModule vkCreateShaderModule = VK_NULL_HANDLE;
        PFN_vkCreatePipelineLayout vkCreatePipelineLayout = VK_NULL_HANDLE;
        PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = VK_NULL_HANDLE;
        PFN_vkDestroyShaderModule vkDestroyShaderModule = VK_NULL_HANDLE;
        PFN_vkCreateFramebuffer vkCreateFramebuffer = VK_NULL_HANDLE;
        PFN_vkCreateCommandPool vkCreateCommandPool = VK_NULL_HANDLE;
        PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = VK_NULL_HANDLE;
        PFN_vkBeginCommandBuffer vkBeginCommandBuffer = VK_NULL_HANDLE;
        PFN_vkEndCommandBuffer vkEndCommandBuffer = VK_NULL_HANDLE;
        PFN_vkCmdSetViewport vkCmdSetViewport = VK_NULL_HANDLE;
        PFN_vkCmdSetScissor vkCmdSetScissor = VK_NULL_HANDLE;
        PFN_vkCmdBeginRenderPass vkCmdBeginRenderPass = VK_NULL_HANDLE;
        PFN_vkCmdEndRenderPass vkCmdEndRenderPass = VK_NULL_HANDLE;
        PFN_vkCmdBindPipeline vkCmdBindPipeline = VK_NULL_HANDLE;
        PFN_vkCmdDraw vkCmdDraw = VK_NULL_HANDLE;
        PFN_vkCreateSemaphore vkCreateSemaphore = VK_NULL_HANDLE;
        PFN_vkCreateFence vkCreateFence = VK_NULL_HANDLE;
        PFN_vkDeviceWaitIdle vkDeviceWaitIdle = VK_NULL_HANDLE;
        PFN_vkDestroyCommandPool vkDestroyCommandPool = VK_NULL_HANDLE;
        PFN_vkDestroyFramebuffer vkDestroyFramebuffer = VK_NULL_HANDLE;
        PFN_vkWaitForFences vkWaitForFences = VK_NULL_HANDLE;
        PFN_vkAcquireNextImageKHR vkAcquireNextImageKHR = VK_NULL_HANDLE;
        PFN_vkResetFences vkResetFences = VK_NULL_HANDLE;
        PFN_vkQueueSubmit vkQueueSubmit = VK_NULL_HANDLE;
        PFN_vkQueuePresentKHR vkQueuePresentKHR = VK_NULL_HANDLE;
        PFN_vkDestroySemaphore vkDestroySemaphore = VK_NULL_HANDLE;
        PFN_vkDestroyFence vkDestroyFence = VK_NULL_HANDLE;
        PFN_vkDestroyPipeline vkDestroyPipeline = VK_NULL_HANDLE;
        PFN_vkDestroyPipelineLayout vkDestroyPipelineLayout = VK_NULL_HANDLE;
        PFN_vkDestroySurfaceKHR vkDestroySurfaceKHR = VK_NULL_HANDLE;
        PFN_vkDestroyRenderPass vkDestroyRenderPass = VK_NULL_HANDLE;
        PFN_vkCreateWaylandSurfaceKHR vkCreateWaylandSurfaceKHR = VK_NULL_HANDLE;
    };

    struct RenderData {
        VkQueue graphics_queue;
        VkQueue present_queue;

        std::vector<VkImage> swapchain_images;
        std::vector<VkImageView> swapchain_image_views;
        std::vector<VkFramebuffer> framebuffers;

        VkRenderPass render_pass;
        VkPipelineLayout pipeline_layout;
        VkPipeline graphics_pipeline;

        VkCommandPool command_pool;
        std::vector<VkCommandBuffer> command_buffers;

        std::vector<VkSemaphore> available_semaphores;
        std::vector<VkSemaphore> finished_semaphore;
        std::vector<VkFence> in_flight_fences;
        std::vector<VkFence> image_in_flight;
        size_t current_frame = 0;
    } render_data_;

    std::string accessToken_;
    int width_;
    int height_;
    std::string assetsPath_;

    struct Init {
        void *window{};
        VulkanLibrary vk_lib;
        vkb::Instance instance;
        VkSurfaceKHR surface{};
        vkb::Device device;
        vkb::Swapchain swapchain;

        // convenience
        VulkanLibrary *operator->() { return &vk_lib; }
    } init_;

    static VkSurfaceKHR createVkSurfaceKHR(Init &init);

    static int device_initialization(Init &init);

    static int create_swapchain(Init &init);

    static int get_queues(Init &init, RenderData &data);

    static int create_render_pass(Init &init, RenderData &data);

    static std::vector<char> readFile(const std::string &filename);

    static VkShaderModule createShaderModule(Init &init,
                                             const std::vector<char> &code);

    static int create_graphics_pipeline(Init &init,
                                        RenderData &data,
                                        std::string &assetsPath);

    static int create_framebuffers(Init &init, RenderData &data);

    static int create_command_pool(Init &init, RenderData &data);

    static int create_command_buffers(Init &init, RenderData &data);

    static int create_sync_objects(Init &init, RenderData &data);

    static int recreate_swapchain(Init &init, RenderData &data);

    static int draw_frame(Init &init, RenderData &data);

    static void cleanup(Init &init, RenderData &data);
};
