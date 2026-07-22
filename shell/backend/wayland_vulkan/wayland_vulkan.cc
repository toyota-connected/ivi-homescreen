/*
 * Copyright 2021-2022 Toyota Connected North America
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

#include "wayland_vulkan.h"
#include "logging/logging.h"

#include <asio/post.hpp>
#include <mutex>

#include <cassert>
#include <cstdlib>
#include <optional>
#include <queue>
#include <string_view>

#include "config/common.h"
#include "engine.h"
#include "logging.h"
#include "profiling/frame_profile.h"
#include "profiling/pv_latency.h"
#include "task_runner.h"
#include "wayland/display.h"

#include <drm_fourcc.h>      // DRM_FORMAT_XRGB8888
#include <wayland-client.h>  // wl_surface_attach/damage_buffer/commit

#include "backend/wayland_vulkan/wl_dmabuf_present.h"
#include "backend/wayland_vulkan/wl_explicit_sync.h"
#if BUILD_HUD
#include "backend/hud/vulkan_hud.h"
#endif
#include "backend/wayland_vulkan/wl_layer_compositor.h"
#include "vulkan_queue_interposer.h"

// The vulkan.hpp dynamic dispatcher needs its storage defined in exactly ONE
// TU per binary. When the drm-kms-vulkan backend is also compiled in, that
// backend's device_caps.cc owns the single definition (it must, since the
// standalone vulkan probe compiles device_caps.cc but not this TU). Suppress
// our copy in that case to avoid a duplicate symbol; vulkan.hpp still declares
// vk::detail::defaultDispatchLoaderDynamic extern, so the reference below
// resolves against device_caps.cc's definition.
#if !BUILD_BACKEND_DRM_KMS_VULKAN
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#endif

// Accessor for the dynamic dispatcher (storage in device_caps.cc). A function
// rather than a namespace-scope reference so there is no static-init-order
// dependency on that other TU's global.
static const auto& d() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

#define S1(x) #x
#define S2(x) S1(x)
#define LOCATION __FILE__ " : " S2(__LINE__)

#define CHECK_VK_RESULT(x)                                         \
  do {                                                             \
    vk::detail::resultCheck(static_cast<vk::Result>(x), LOCATION); \
  } while (0)

WaylandVulkanBackend::WaylandVulkanBackend(Display* shell_display,
                                           wl_display* display,
                                           const uint32_t width,
                                           const uint32_t height,
                                           const bool enable_validation_layers)
    : Backend(),
      enable_validation_layers_(enable_validation_layers),
      resize_pending_(false),
      wl_display_(display),
      width_(width),
      height_(height) {
  shell_display_ = shell_display;
  if (shell_display != nullptr) {
    // wp_presentation + vsync feedback machinery live in the Display-owned
    // provider; the backend forwards to it.
    vsync_ = shell_display->GetVsyncProvider();
    if (vsync_ != nullptr && !vsync_->Usable()) {
      ihs::log::info(
          "[WaylandVulkanBackend] wp_presentation unusable — vsync_callback "
          "disabled, falling back to engine wall-clock scheduler");
    }
  }
  VULKAN_HPP_DEFAULT_DISPATCHER.init();
  createInstance();
  setupDebugMessenger();
}

FlutterRendererConfig WaylandVulkanBackend::GetRenderConfig() {
  return {
      .type = kVulkan,
      .vulkan{
          .struct_size = sizeof(FlutterRendererConfig),
          .version = VK_MAKE_VERSION(1, 1, 0),
          .instance = instance_,
          .physical_device = physical_device_,
          .device = device_,
          .queue_family_index = queue_family_index_,
          .queue = queue_,
          .enabled_instance_extension_count =
              enabled_instance_extensions_.size(),
          .enabled_instance_extensions = enabled_instance_extensions_.data(),
          .enabled_device_extension_count = enabled_device_extensions_.size(),
          .enabled_device_extensions = enabled_device_extensions_.data(),
          .get_instance_proc_address_callback = GetInstanceProcAddressCallback,
          .get_next_image_callback = GetNextImageCallback,
          .present_image_callback = PresentCallback,
      }};
}

bool WaylandVulkanBackend::GetVulkanContext(BackendVulkanContext* out) const {
  if (out == nullptr || device_ == VK_NULL_HANDLE) {
    return false;
  }
  out->instance = instance_;
  out->physical_device = physical_device_;
  out->device = device_;
  out->queue = queue_;
  out->queue_family_index = queue_family_index_;
  // The INTERPOSED loader (not the raw d().vkGetInstanceProcAddr): a plugin
  // reusing the shared queue must resolve vkQueue* through the locking
  // trampolines, or its submits race the engine's and this backend's on the one
  // graphics queue (issue #208) — exactly the threading error a raw loader
  // produces.
  out->get_instance_proc_addr =
      reinterpret_cast<void*>(&WaylandVulkanBackend::PluginInstanceProcAddr);
  // enabled_device_extensions_ outlives every context handed out (it is set
  // once at device creation), so exposing its storage directly is safe.
  out->device_extensions = enabled_device_extensions_.data();
  out->device_extension_count = enabled_device_extensions_.size();
  return true;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
WaylandVulkanBackend::PluginInstanceProcAddr(VkInstance instance,
                                             const char* procname) {
  // Same shim as GetInstanceProcAddressCallback (the engine's loader), in the
  // PFN_vkGetInstanceProcAddr shape a plugin calls: swap vkQueue* (and
  // vkGetDeviceProcAddr) for the shared-queue locking trampolines, else fall
  // through to the real loader.
  if (auto* interposed = wayland_vulkan::QueueInterposer::Interpose(
          instance, procname, d().vkGetInstanceProcAddr)) {
    return interposed;
  }
  return d().vkGetInstanceProcAddr(instance, procname);
}

FlutterCompositor WaylandVulkanBackend::GetCompositorConfig() {
#if BUILD_COMPOSITOR
  return {
      .struct_size = sizeof(FlutterCompositor),
      .user_data = this,
      .create_backing_store_callback = CreateBackingStore,
      .collect_backing_store_callback = CollectBackingStore,
      .present_layers_callback = PresentLayers,
      .avoid_backing_store_cache = false,
      .present_view_callback = nullptr,
  };
#else
  return {
      .struct_size = sizeof(FlutterCompositor),
      .user_data = this,
      .create_backing_store_callback = nullptr,
      .collect_backing_store_callback = nullptr,
      .present_layers_callback = nullptr,
      .avoid_backing_store_cache = true,
      .present_view_callback = nullptr,
  };
#endif
}

WaylandVulkanBackend::~WaylandVulkanBackend() {
  // Session-aggregate profile summary (IVI_VK_PROFILE=1). Logged from
  // the dtor so it captures the whole run regardless of which present
  // path was active. No-op when the env-var was unset (counters never
  // accumulated).
  const auto& s = session_totals_;
  if (s.presented_frames > 0) {
    const uint32_t samples =
        (s.interval_sum_ns > 0) ? s.presented_frames - 1 : s.presented_frames;
    const uint64_t mean_ns = samples > 0 ? s.interval_sum_ns / samples : 0;
    const double fps = mean_ns > 0 ? 1e9 / static_cast<double>(mean_ns) : 0.0;
    const uint32_t total = s.bucket_60hz + s.bucket_30hz + s.bucket_20hz +
                           s.bucket_slow + s.bucket_idle;
    const uint64_t s_lat_mean_us =
        s.pipeline_latency_samples > 0
            ? (s.pipeline_latency_sum_ns / s.pipeline_latency_samples) / 1000
            : 0;
    ihs::log::info(
        "[WaylandVulkanBackend] session summary: frames={} fps={:.2f} "
        "mean_interval={}us max_interval={}us present_failures={} "
        "discarded={} flags=0x{:x} pipeline_mean={}us pipeline_max={}us",
        s.presented_frames, fps, mean_ns / 1000, s.interval_max_ns / 1000,
        s.present_failures, s.discarded_frames, s.flags_or, s_lat_mean_us,
        s.pipeline_latency_max_ns / 1000);
    if (total > 0) {
      const double inv = 100.0 / static_cast<double>(total);
      ihs::log::info(
          "[WaylandVulkanBackend] session buckets: "
          "60Hz(≤17ms)={} ({:.1f}%) 30Hz(18-33ms)={} ({:.1f}%) "
          "20Hz(34-50ms)={} ({:.1f}%) slow(51-100ms)={} ({:.1f}%) "
          "idle(>100ms)={} ({:.1f}%)",
          s.bucket_60hz, s.bucket_60hz * inv, s.bucket_30hz,
          s.bucket_30hz * inv, s.bucket_20hz, s.bucket_20hz * inv,
          s.bucket_slow, s.bucket_slow * inv, s.bucket_idle,
          s.bucket_idle * inv);
    }
  }

  if (device_ != nullptr) {
    // Drain all in-flight GPU work before tearing anything down. Without this
    // the swapchain (and the WSI's wl_buffer proxies bound to its images) can
    // still be in use, which both the validation layers and Mesa's Wayland WSI
    // flag ("wl_buffer still attached" / "queue destroyed while proxies still
    // attached") and which can fault during vkDestroyDevice.
    d().vkDeviceWaitIdle(device_);
#if BUILD_HUD
    // Tear the HUD down while device_ is still valid: ~VulkanHud runs
    // ImGui_ImplVulkan_Shutdown, which frees Vulkan buffers/memory. Left to the
    // member destructor it would run after vkDestroyDevice below and fault.
    hud_.reset();
#endif
#if BUILD_COMPOSITOR
    CompositorPipeliningCleanup();
    // Retire any in-flight pacing callback. Safe here: App::~App() has already
    // called Display::StopEvents() on every display, so the reactor is no
    // longer dispatching Wayland events and OnFrameDone cannot race this — but
    // the connection is still up (wl_display_disconnect runs later, in
    // ~Display), so the proxy destroy is valid. Same teardown ordering the
    // vsync feedback proxies rely on.
    {
      std::lock_guard<std::mutex> lock(frame_mu_);
      if (frame_callback_ != nullptr) {
        wl_callback_destroy(frame_callback_);
        frame_callback_ = nullptr;
      }
    }
    // Tear down explicit sync before the device: it destroys its acquire
    // timeline VkSemaphore with device_ (safe after the vkDeviceWaitIdle above)
    // and its wp_linux_drm_syncobj proxies while the connection is still up.
    explicit_sync_.reset();
    // Layer compositor owns Vulkan objects (render pass, pipeline, pools, ...);
    // destroy before the device.
    layer_compositor_.reset();
    // Tear down the dma-buf present ring before the device — DmabufImage's
    // destructor destroys its image view/image/memory with device_, and the
    // command buffers belong to dmabuf_cmd_pool_.
    for (auto& s : dmabuf_slots_) {
      if (s.fence != VK_NULL_HANDLE) {
        d().vkDestroyFence(device_, s.fence, nullptr);
      }
    }
    dmabuf_slots_.clear();
    if (dmabuf_cmd_pool_ != VK_NULL_HANDLE) {
      d().vkDestroyCommandPool(device_, dmabuf_cmd_pool_, nullptr);
      dmabuf_cmd_pool_ = VK_NULL_HANDLE;
    }
    // Destroy the backing stores while device_ is still valid. These pools are
    // members, so their default destruction runs AFTER this dtor body — i.e.
    // after vkDestroyDevice below — and VulkanBackingStore::~ calls
    // vkDestroyImageView with the (by then invalid) device, aborting in the
    // loader. Flushing here fixes both present paths (a SIGTERM mid-run leaves
    // stores checked out).
    m_alive_stores.clear();
    m_store_pool.Flush();
    // Imported platform-view acquire semaphores (queue is idle by here).
    for (auto& [seq, sem] : acquire_wait_retire_) {
      d().vkDestroySemaphore(device_, sem, nullptr);
    }
    acquire_wait_retire_.clear();
    for (VkSemaphore s : frame_acquire_waits_) {
      d().vkDestroySemaphore(device_, s, nullptr);
    }
    frame_acquire_waits_.clear();
    if (release_sem_ != VK_NULL_HANDLE) {
      d().vkDestroySemaphore(device_, release_sem_, nullptr);
      release_sem_ = VK_NULL_HANDLE;
    }
    // Run any imports a late dispose deferred but present never reaped (GPU is
    // idle from the vkDeviceWaitIdle above, so this is safe).
    {
      const std::lock_guard<std::mutex> lock(deferred_destroy_mu_);
      for (auto& [tag, fn] : deferred_destroys_) {
        if (fn) {
          fn();
        }
      }
      deferred_destroys_.clear();
    }
#endif
    if (swapchain_command_pool_ != nullptr) {
      d().vkDestroyCommandPool(device_, swapchain_command_pool_, nullptr);
    }
    for (auto sem : present_transition_semaphores_) {
      if (sem != nullptr) {
        d().vkDestroySemaphore(device_, sem, nullptr);
      }
    }
    present_transition_semaphores_.clear();
    if (image_ready_fence_ != nullptr) {
      d().vkDestroyFence(device_, image_ready_fence_, nullptr);
    }
    // Destroy the swapchain before the device. It is otherwise only torn down
    // on resize (InitializeSwapChain); on shutdown it must be released here so
    // the WSI hands its images' wl_buffers back to the compositor. The
    // per-image color-attachment views go first (queue already idle above).
    for (auto view : swapchain_image_views_) {
      if (view != VK_NULL_HANDLE) {
        d().vkDestroyImageView(device_, view, nullptr);
      }
    }
    swapchain_image_views_.clear();
    if (swapchain_ != nullptr) {
      d().vkDestroySwapchainKHR(device_, swapchain_, nullptr);
      swapchain_ = VK_NULL_HANDLE;
    }
    // The engine has been shut down and the queue drained
    // (vkDeviceWaitIdle above); drop the interposer registration before the
    // queue's device — and with it the queue handle — goes away. Any
    // stale-handle call after this point (there should be none) passes
    // through unlocked rather than dereferencing a dead mutex mapping.
    wayland_vulkan::QueueInterposer::UnregisterQueue(queue_);
    d().vkDestroyDevice(device_, nullptr);
  }
  if (surface_ != nullptr) {
    d().vkDestroySurfaceKHR(instance_, surface_, nullptr);
  }
  if (enable_validation_layers_) {
    if (mDebugCallback) {
      d().vkDestroyDebugReportCallbackEXT(instance_, mDebugCallback, VKALLOC);
    }
    if (mDebugMessenger) {
      d().vkDestroyDebugUtilsMessengerEXT(instance_, mDebugMessenger, VKALLOC);
    }
  }
  if (instance_ != nullptr) {
    d().vkDestroyInstance(instance_, nullptr);
  }
  if (!enabled_instance_extensions_.empty()) {
    for (const auto it : enabled_instance_extensions_) {
      free((void*)it);  // strdup'd in createInstance(); owned, must be freed
    }
  }
  // NB: enabled_layer_extensions_ is NOT freed. Unlike the instance extensions
  // (strdup'd), its only entry is the constexpr string literal
  // VK_LAYER_KHRONOS_VALIDATION_NAME pushed in createInstance() — not heap
  // allocated. free()ing it aborted the process (free(): invalid size) at
  // teardown, but only when validation layers were enabled (the sole path that
  // populates this vector), which is why it surfaced only under -d / -d-style
  // runs.
}

void WaylandVulkanBackend::createInstance() {
  debugUtilsSupported_ = false;
  surfaceSupported_ = false;
  waylandSurfaceSupported_ = false;
  auto instance_extensions = vk::enumerateInstanceExtensionProperties();
  ihs::log::debug("Vulkan Instance Extensions:");

  for (const auto& l : instance_extensions.value) {
    ihs::log::debug("\t{}, version: {}", l.extensionName.data(), l.specVersion);
    if (enable_validation_layers_) {
      if (strcmp(l.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) ==
          0) {
        enabled_instance_extensions_.push_back(strdup(l.extensionName));
      }
      if (strcmp(l.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) {
        debugUtilsSupported_ = true;
        enabled_instance_extensions_.push_back(strdup(l.extensionName));
      }
      if (strcmp(l.extensionName, VK_EXT_DEBUG_REPORT_EXTENSION_NAME) == 0) {
        enabled_instance_extensions_.push_back(strdup(l.extensionName));
      }
    }
    if (strcmp(l.extensionName, VK_KHR_SURFACE_EXTENSION_NAME) == 0) {
      surfaceSupported_ = true;
      enabled_instance_extensions_.push_back(strdup(l.extensionName));
    }
    if (strcmp(l.extensionName, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME) == 0) {
      waylandSurfaceSupported_ = true;
      enabled_instance_extensions_.push_back(strdup(l.extensionName));
    }
  }

  if (!surfaceSupported_ || !waylandSurfaceSupported_) {
    ihs::log::critical(
        "This Vulkan driver does not support the minimum required extensions");
    exit(EXIT_FAILURE);
  }

  std::stringstream ss;
  ss << "Enabling " << enabled_instance_extensions_.size()
     << " instance extensions:";
  for (auto& extension : enabled_instance_extensions_) {
    ss << "\n\t" << extension;
  }
  ihs::log::info(ss.str());

  VkApplicationInfo app_info{};
  app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app_info.pApplicationName = kApplicationName;
  app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.pEngineName = "No Engine";
  app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
  app_info.apiVersion = VK_MAKE_VERSION(1, 1, 0);

  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app_info;
  info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_instance_extensions_.size());
  info.ppEnabledExtensionNames = enabled_instance_extensions_.data();

  if (enable_validation_layers_) {
    constexpr char layer_name[] = "VK_LAYER_KHRONOS_validation";
    constexpr VkBool32 setting_validate_core = VK_TRUE;
    constexpr VkBool32 setting_validate_sync = VK_TRUE;
    constexpr VkBool32 setting_thread_safety = VK_TRUE;
    const char* setting_debug_action[] = {"VK_DBG_LAYER_ACTION_LOG_MSG"};
    const char* setting_report_flags[] = {"error", "warn", "info", "perf",
                                          "verbose"};
    constexpr VkBool32 setting_enable_message_limit = VK_TRUE;
    constexpr uint32_t setting_duplicate_message_limit = 3;

    const VkLayerSettingEXT settings[] = {
        {layer_name, "validate_core", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1,
         &setting_validate_core},
        {layer_name, "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1,
         &setting_validate_sync},
        {layer_name, "thread_safety", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1,
         &setting_thread_safety},
        {layer_name, "debug_action", VK_LAYER_SETTING_TYPE_STRING_EXT, 1,
         setting_debug_action},
        {layer_name, "report_flags", VK_LAYER_SETTING_TYPE_STRING_EXT,
         static_cast<uint32_t>(std::size(setting_report_flags)),
         setting_report_flags},
        {layer_name, "enable_message_limit", VK_LAYER_SETTING_TYPE_BOOL32_EXT,
         1, &setting_enable_message_limit},
        {layer_name, "duplicate_message_limit",
         VK_LAYER_SETTING_TYPE_UINT32_EXT, 1, &setting_duplicate_message_limit},
    };

    VkLayerSettingsCreateInfoEXT layer_settings_create_info{};
    layer_settings_create_info.sType =
        VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    layer_settings_create_info.settingCount =
        static_cast<uint32_t>(std::size(settings));
    layer_settings_create_info.pSettings = settings;

    info.pNext = &layer_settings_create_info;
  }

  constexpr char VK_LAYER_KHRONOS_VALIDATION_NAME[] =
      "VK_LAYER_KHRONOS_validation";

  const auto available_layers = vk::enumerateInstanceLayerProperties();
  if (!available_layers.value.empty()) {
    ihs::log::debug("Vulkan Instance Layers:");
    for (const auto& l : available_layers.value) {
      ihs::log::debug("\t{} - {}", l.layerName.data(), l.description.data());
      if (enable_validation_layers_ &&
          strcmp(l.layerName, VK_LAYER_KHRONOS_VALIDATION_NAME) == 0) {
        enabled_layer_extensions_.push_back(VK_LAYER_KHRONOS_VALIDATION_NAME);
        break;
      }
    }
  }

  if (!enabled_layer_extensions_.empty()) {
    ss.clear();
    ss.str("");
    ss << "Enabling " << enabled_layer_extensions_.size()
       << " layer extensions:";
    for (const auto& layer : enabled_layer_extensions_) {
      ss << "\n\t" << layer;
    }
    ihs::log::info(ss.str());
  }

  info.enabledLayerCount =
      static_cast<uint32_t>(enabled_layer_extensions_.size());
  info.ppEnabledLayerNames = enabled_layer_extensions_.data();

  CHECK_VK_RESULT(d().vkCreateInstance(&info, nullptr, &instance_) !=
                  VK_SUCCESS);

  VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance_));
}

void WaylandVulkanBackend::setupDebugMessenger() {
  if (!enable_validation_layers_)
    return;

  if (debugUtilsSupported_) {
    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugUtilsCallback;

    CHECK_VK_RESULT(d().vkCreateDebugUtilsMessengerEXT(
                        instance_, &createInfo, VKALLOC, &mDebugMessenger) !=
                    VK_SUCCESS);
  } else if (d().vkCreateDebugReportCallbackEXT) {
    VkDebugReportCallbackCreateInfoEXT cb_info{};
    cb_info.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
    cb_info.flags =
        VK_DEBUG_REPORT_WARNING_BIT_EXT | VK_DEBUG_REPORT_ERROR_BIT_EXT;
    cb_info.pfnCallback = debugReportCallback;
    CHECK_VK_RESULT(d().vkCreateDebugReportCallbackEXT(
                        instance_, &cb_info, VKALLOC, &mDebugCallback) !=
                    VK_SUCCESS);
  }
}

void WaylandVulkanBackend::findPhysicalDevice() {
  uint32_t count;
  CHECK_VK_RESULT(d().vkEnumeratePhysicalDevices(instance_, &count, nullptr));
  std::vector<VkPhysicalDevice> physical_devices(count);
  CHECK_VK_RESULT(d().vkEnumeratePhysicalDevices(instance_, &count,
                                                 physical_devices.data()));

  IHS_DEBUG("Enumerating {} physical device(s).", count);

  uint32_t selected_score = 0;
  for (const auto& physical_device : physical_devices) {
    VkPhysicalDeviceProperties properties;
    VkPhysicalDeviceFeatures features;
    d().vkGetPhysicalDeviceProperties(physical_device, &properties);
    d().vkGetPhysicalDeviceFeatures(physical_device, &features);

    IHS_DEBUG("Checking device: {}", properties.deviceName);

    uint32_t score = 0;
    std::vector<const char*> supported_extensions;

    uint32_t qfp_count;
    d().vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &qfp_count,
                                                 nullptr);
    std::vector<VkQueueFamilyProperties> qfp(qfp_count);
    d().vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &qfp_count,
                                                 qfp.data());
    std::optional<uint32_t> graphics_queue_family;
    for (uint32_t i = 0; i < qfp.size(); i++) {
      // Only pick graphics queues that can also present to the surface.
      // Graphics queues that can't present are rare if not nonexistent, but
      // the spec allows for this, so check it anyhow.
      VkBool32 surface_present_supported;
      CHECK_VK_RESULT(d().vkGetPhysicalDeviceSurfaceSupportKHR(
          physical_device, i, surface_, &surface_present_supported));

      if (!graphics_queue_family.has_value() &&
          qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT &&
          surface_present_supported) {
        graphics_queue_family = i;
      }
    }

    // Skip physical devices that don't have a graphics queue.
    if (!graphics_queue_family.has_value()) {
      ihs::log::info("  - Skipping due to no suitable graphics queues.");
      continue;
    }

    // Prefer discrete GPUs.
    if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      score += 1 << 30;
    }

    uint32_t extension_count;
    CHECK_VK_RESULT(d().vkEnumerateDeviceExtensionProperties(
        physical_device, nullptr, &extension_count, nullptr));
    std::vector<VkExtensionProperties> available_extensions(extension_count);
    CHECK_VK_RESULT(d().vkEnumerateDeviceExtensionProperties(
        physical_device, nullptr, &extension_count,
        available_extensions.data()));

    bool supports_swap_chain = false;
    for (const auto& [extensionName, specVersion] : available_extensions) {
      if (strcmp(VK_KHR_SWAPCHAIN_EXTENSION_NAME, extensionName) == 0) {
        supports_swap_chain = true;
        supported_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
      }
      // The spec requires VK_KHR_portability_subset be enabled whenever it's
      // available on a device. It's present on compatibility ICDs like
      // MoltenVK.
      else if (strcmp("VK_KHR_portability_subset", extensionName) == 0) {
        supported_extensions.push_back("VK_KHR_portability_subset");
      }
      // Prefer GPUs that support VK_KHR_get_memory_requirements2.
      else if (strcmp(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                      extensionName) == 0) {
        score += 1 << 29;
        supported_extensions.push_back(
            VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
      }
      // dma-buf zero-copy present path: import/export a modifier-tiled image
      // the compositor can scan out. Enabled when advertised; the path stays
      // gated at runtime on a successful DmabufImage allocation, so a device
      // missing any of these simply falls back to the WSI swapchain. The last
      // four back the wp_linux_drm_syncobj explicit-sync path (a timeline
      // semaphore imported from a DRM syncobj + the render-node resolve);
      // absent, that path degrades to the CPU fence.
      else {
        for (const char* want :
             {VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
              VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
              VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
              VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
              VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
              VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
              VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
              // Lets a platform-view engine (e.g. the Mapbox Maps SDK) create a
              // pipeline cache with EXTERNALLY_SYNCHRONIZED on
              // this 1.1-targeted device without a validation error. Core in
              // Vulkan 1.3; enabled as an extension here when advertised.
              // Harmless otherwise.
              VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME,
              VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME}) {
          if (strcmp(want, extensionName) == 0) {
            supported_extensions.push_back(want);
          }
        }
      }
    }

    // Skip physical devices that don't have swap chain support.
    if (!supports_swap_chain) {
      IHS_DEBUG("  - Skipping due to lack of swap chain support.");
      continue;
    }

    // Prefer GPUs with larger max texture sizes.
    score += properties.limits.maxImageDimension2D;

    if (selected_score < score) {
      IHS_DEBUG("  - This is the best device so far. Score: 0x{:x}", score);

      selected_score = score;
      physical_device_ = physical_device;
      enabled_device_extensions_ = supported_extensions;
      queue_family_index_ =
          graphics_queue_family.value_or(std::numeric_limits<uint32_t>::max());

      // Bingo, we finally found a physical device that supports everything we
      // need.
      d().vkGetPhysicalDeviceFeatures(physical_device,
                                      &physical_device_features_);
      d().vkGetPhysicalDeviceMemoryProperties(
          physical_device, &physical_device_memory_properties_);

      // Print some driver or MoltenVK information if it is available.
      if (d().vkGetPhysicalDeviceProperties2KHR) {
        VkPhysicalDeviceDriverProperties driverProperties{};
        driverProperties.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;

        VkPhysicalDeviceProperties2 physicalDeviceProperties2{};
        physicalDeviceProperties2.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        physicalDeviceProperties2.pNext = &driverProperties;

        d().vkGetPhysicalDeviceProperties2KHR(physical_device_,
                                              &physicalDeviceProperties2);
        ihs::log::info("Vulkan device driver: {} {}",
                       driverProperties.driverName,
                       driverProperties.driverInfo);
      }

      // Print out some properties of the GPU for diagnostic purposes.
      ihs::log::info("vendor {:x}, device {:x}, driver {:x}, api {}.{}",
                     properties.vendorID, properties.deviceID,
                     properties.driverVersion,
                     VK_VERSION_MAJOR(properties.apiVersion),
                     VK_VERSION_MINOR(properties.apiVersion));
      break;
    }
  }

  if (physical_device_ == nullptr) {
    ihs::log::critical("Failed to find a compatible Vulkan physical device.");
    exit(EXIT_FAILURE);
  }
}

void WaylandVulkanBackend::createLogicalDevice() {
#if !defined(NDEBUG)
  std::stringstream ss;
  ss << "Enabling " << enabled_device_extensions_.size()
     << " device extensions:";
  for (const char* extension : enabled_device_extensions_) {
    ss << "  - " << extension;
  }
  IHS_DEBUG(ss.str());
#endif

  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue_info{};
  queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queue_info.queueFamilyIndex = queue_family_index_;
  queue_info.queueCount = 1;
  queue_info.pQueuePriorities = &priority;

  VkPhysicalDeviceFeatures device_features{};
  {
    // The Mapbox Maps SDK (and other platform-view engines) create anisotropic
    // samplers and use dual-source blending; enable those features when the
    // device offers them so such an engine stays validation-clean. Both are
    // widely supported and harmless to the rest of the compositor.
    VkPhysicalDeviceFeatures supported{};
    d().vkGetPhysicalDeviceFeatures(physical_device_, &supported);
    device_features.samplerAnisotropy = supported.samplerAnisotropy;
    device_features.dualSrcBlend = supported.dualSrcBlend;
  }
  VkDeviceCreateInfo device_info{};
  device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  device_info.queueCreateInfoCount = 1;
  device_info.pQueueCreateInfos = &queue_info;
  device_info.enabledExtensionCount =
      static_cast<uint32_t>(enabled_device_extensions_.size());
  device_info.ppEnabledExtensionNames = enabled_device_extensions_.data();
  device_info.pEnabledFeatures = &device_features;

  // Enable the timeline-semaphore feature when the extension was selected, so
  // the wp_linux_drm_syncobj explicit-sync path can create the timeline
  // semaphore it imports the acquire DRM syncobj into. Harmless otherwise.
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline_feature{};
  timeline_feature.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
  for (const char* ext : enabled_device_extensions_) {
    if (strcmp(ext, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) == 0) {
      timeline_feature.timelineSemaphore = VK_TRUE;
      device_info.pNext = &timeline_feature;
      break;
    }
  }

  // Enable pipelineCreationCacheControl when the extension was selected, so a
  // platform-view engine's externally-synchronized pipeline cache is valid.
  // The extension being advertised does not guarantee the feature bit, so
  // query it via vkGetPhysicalDeviceFeatures2 and only chain it when the
  // device reports support — requesting an unsupported feature can fail
  // vkCreateDevice.
  VkPhysicalDevicePipelineCreationCacheControlFeatures cache_control{};
  cache_control.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES;
  for (const char* ext : enabled_device_extensions_) {
    if (strcmp(ext, VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME) ==
        0) {
      VkPhysicalDevicePipelineCreationCacheControlFeatures cache_supported{};
      cache_supported.sType =
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES;
      VkPhysicalDeviceFeatures2 features2{};
      features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
      features2.pNext = &cache_supported;
      d().vkGetPhysicalDeviceFeatures2(physical_device_, &features2);
      if (cache_supported.pipelineCreationCacheControl == VK_TRUE) {
        cache_control.pipelineCreationCacheControl = VK_TRUE;
        // chain ahead of any prior struct (pNext on the create info is const)
        cache_control.pNext = const_cast<void*>(device_info.pNext);
        device_info.pNext = &cache_control;
      }
      break;
    }
  }

  CHECK_VK_RESULT(
      d().vkCreateDevice(physical_device_, &device_info, nullptr, &device_));

  d().vkGetDeviceQueue(device_, queue_family_index_, 0, &queue_);

  // GetRenderConfig() hands queue_ to the Flutter engine, whose raster
  // thread submits on it concurrently with this backend's platform-thread
  // present/transfer paths. Register the queue so the interposed vkQueue*
  // entry points handed out by GetInstanceProcAddressCallback serialize the
  // engine's submissions on the same queue_mutex_ the backend already
  // takes. See issue #208.
  wayland_vulkan::QueueInterposer::RegisterQueue(queue_, &queue_mutex_);
}

bool WaylandVulkanBackend::InitializeSwapChain() {
  if (resize_pending_) {
    resize_pending_ = false;
    d().vkDestroySwapchainKHR(device_, swapchain_, nullptr);

    {
      std::lock_guard<std::mutex> queue_lock(queue_mutex_);
      CHECK_VK_RESULT(d().vkQueueWaitIdle(queue_));
    }
    // Queue is idle: drop the old swapchain image views before they are
    // recreated for the new image set below, and the layer compositor's
    // framebuffers built from them (else it would reuse a framebuffer that
    // references a freed view).
    for (auto view : swapchain_image_views_) {
      if (view != VK_NULL_HANDLE) {
        d().vkDestroyImageView(device_, view, nullptr);
      }
    }
    swapchain_image_views_.clear();
#if BUILD_COMPOSITOR
    if (layer_compositor_) {
      layer_compositor_->ClearFramebuffers();
    }
#endif
#if BUILD_HUD
    if (hud_) {
      hud_->DropFramebuffers();  // views it cached were just freed
    }
#endif
    CHECK_VK_RESULT(
        d().vkResetCommandPool(device_, swapchain_command_pool_,
                               VK_COMMAND_POOL_RESET_RELEASE_RESOURCES_BIT));
    // The queue is idle here; tear down the old per-image present-transition
    // semaphores so they can be recreated for the new image count below.
    for (auto sem : present_transition_semaphores_) {
      if (sem != nullptr) {
        d().vkDestroySemaphore(device_, sem, nullptr);
      }
    }
    present_transition_semaphores_.clear();
  }

  // --------------------------------------------------------------------------
  // Choose an image format that can be presented to the surface, preferring
  // the common BGRA+sRGB if available.
  // --------------------------------------------------------------------------

  uint32_t format_count;
  // First touch on the Wayland-backed VkSurfaceKHR. Mesa's WSI surfaces a
  // missing GPU-buffer-allocation protocol (zwp_linux_dmabuf_v1 / wl_drm)
  // as VK_ERROR_SURFACE_LOST_KHR on this call — that's the root cause when
  // the compositor was launched without dmabuf support. Soft-fail rather
  // than asserting so CreateSurface can exit cleanly with a clear message.
  if (const auto r =
          static_cast<vk::Result>(d().vkGetPhysicalDeviceSurfaceFormatsKHR(
              physical_device_, surface_, &format_count, nullptr));
      r != vk::Result::eSuccess) {
    ihs::log::critical(
        "vkGetPhysicalDeviceSurfaceFormatsKHR failed: {}. On Wayland this "
        "usually means the compositor exposes neither zwp_linux_dmabuf_v1 "
        "nor wl_drm — Mesa Vulkan WSI cannot back the swapchain.",
        vk::to_string(r));
    return false;
  }
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  CHECK_VK_RESULT(d().vkGetPhysicalDeviceSurfaceFormatsKHR(
      physical_device_, surface_, &format_count, formats.data()));

  surface_format_ = formats[0];
  for (const auto& format : formats) {
    if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
        format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      surface_format_ = format;
      break;
    }
  }

  // --------------------------------------------------------------------------
  // Choose the presentable image size that's as close as possible to the
  // window size.
  // --------------------------------------------------------------------------

  VkExtent2D clientSize;

  VkSurfaceCapabilitiesKHR surface_capabilities;
  CHECK_VK_RESULT(d().vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
      physical_device_, surface_, &surface_capabilities));

  if (surface_capabilities.currentExtent.width != UINT32_MAX) {
    // If the surface reports a specific extent, we must use it.
    clientSize = surface_capabilities.currentExtent;
  } else {
    VkExtent2D actual_extent{};
    actual_extent.width = width_;
    actual_extent.height = height_;

    clientSize.width =
        std::max(surface_capabilities.minImageExtent.width,
                 std::min(surface_capabilities.maxImageExtent.width,
                          actual_extent.width));
    clientSize.height =
        std::max(surface_capabilities.minImageExtent.height,
                 std::min(surface_capabilities.maxImageExtent.height,
                          actual_extent.height));
  }

  // --------------------------------------------------------------------------
  // Desired image count
  // --------------------------------------------------------------------------

  const uint32_t maxImageCount = surface_capabilities.maxImageCount;
  const uint32_t minImageCount = surface_capabilities.minImageCount;
  uint32_t desiredImageCount = minImageCount + 1;

  // According to section 30.5 of VK 1.1, maxImageCount of zero means "that
  // there is no limit on the number of images, though there may be limits
  // related to the total amount of memory used by presentable images."
  if (maxImageCount != 0 && desiredImageCount > maxImageCount) {
    ihs::log::error("Swap chain does not support {} images.",
                    desiredImageCount);
    desiredImageCount = surface_capabilities.minImageCount;
  }

  // --------------------------------------------------------------------------
  // Choose the present mode.
  // --------------------------------------------------------------------------

  uint32_t mode_count;
  CHECK_VK_RESULT(d().vkGetPhysicalDeviceSurfacePresentModesKHR(
      physical_device_, surface_, &mode_count, nullptr));
  std::vector<VkPresentModeKHR> modes(mode_count);
  CHECK_VK_RESULT(d().vkGetPhysicalDeviceSurfacePresentModesKHR(
      physical_device_, surface_, &mode_count, modes.data()));
  assert(!formats.empty());  // Shouldn't be possible.

  // Default = FIFO (strict vblank gating, the spec-mandated mode). Env
  // override IVI_VK_PRESENT_MODE picks an alternative IFF the compositor
  // advertises it; otherwise warn and fall back to FIFO. MAILBOX is the
  // interesting case for vsync_callback profiling: it lets the rasterizer
  // outpace scanout, weakening Mesa's WSI backpressure so Flutter's
  // own scheduling drift becomes visible at the present-interval layer.
  VkPresentModeKHR requested_mode = kPreferredPresentMode;
  if (const char* env = std::getenv("IVI_VK_PRESENT_MODE"); env != nullptr) {
    const std::string_view name(env);
    if (name == "mailbox") {
      requested_mode = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (name == "fifo_relaxed") {
      requested_mode = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    } else if (name == "immediate") {
      requested_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    } else if (name != "fifo") {
      ihs::log::warn(
          "[WaylandVulkanBackend] IVI_VK_PRESENT_MODE='{}' unrecognized; "
          "valid: fifo|fifo_relaxed|mailbox|immediate. Falling back to fifo.",
          env);
    }
  }
  VkPresentModeKHR present_mode = modes[0];
  bool got_requested = false;
  for (const auto& mode : modes) {
    if (mode == requested_mode) {
      present_mode = mode;
      got_requested = true;
      break;
    }
  }
  if (!got_requested && requested_mode != kPreferredPresentMode) {
    // Requested mode wasn't advertised — fall back to FIFO if present
    // (it's required by the spec for any compositor that supports a
    // swapchain at all), else first available.
    for (const auto& mode : modes) {
      if (mode == kPreferredPresentMode) {
        present_mode = mode;
        break;
      }
    }
    ihs::log::warn(
        "[WaylandVulkanBackend] requested present mode {} not advertised "
        "by compositor; using {}.",
        static_cast<int>(requested_mode), static_cast<int>(present_mode));
  }
  ihs::log::info("[WaylandVulkanBackend] present mode: {}",
                 static_cast<int>(present_mode));

  // --------------------------------------------------------------------------
  // Create the swap chain.
  // --------------------------------------------------------------------------
  VkSwapchainCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  info.surface = surface_;
  info.minImageCount = desiredImageCount;
  info.imageFormat = surface_format_.format;
  info.imageColorSpace = surface_format_.colorSpace;
  info.imageExtent = clientSize;
  swapchain_extent_ =
      clientSize;  // the extent the images (and views) are made at
  info.imageArrayLayers = 1;
  // The compositor present path (PresentLayersImpl/BlitStoreToSwapchain) blits
  // each backing store into the swapchain image, so it must be a transfer
  // destination — not just a color attachment. Request TRANSFER_DST_BIT in
  // addition; gate it on the surface's advertised usage so creation can't fail
  // on a compositor that doesn't support it (COLOR_ATTACHMENT_BIT is always
  // supported per spec).
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if (surface_capabilities.supportedUsageFlags &
      VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
    info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = surface_capabilities.currentTransform;
  info.compositeAlpha = (surface_capabilities.supportedCompositeAlpha &
                         VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                            ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                            : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = present_mode;
  info.clipped = VK_TRUE;

  auto result = d().vkCreateSwapchainKHR(device_, &info, VKALLOC, &swapchain_);
  CHECK_VK_RESULT(result);
  if (result != VK_SUCCESS) {
    return false;
  }

  // --------------------------------------------------------------------------
  // Fetch swap chain images
  // --------------------------------------------------------------------------

  uint32_t image_count;
  CHECK_VK_RESULT(
      d().vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr));
  swapchain_images_.resize(image_count);
  CHECK_VK_RESULT(d().vkGetSwapchainImagesKHR(device_, swapchain_, &image_count,
                                              swapchain_images_.data()));

  // Color-attachment views of the swapchain images, so the layer stack can be
  // alpha-blended straight into the swapchain on the WSI path (platform-view
  // frames). Images already carry VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT above.
  swapchain_image_views_.resize(swapchain_images_.size());
  for (size_t i = 0; i < swapchain_images_.size(); i++) {
    VkImageViewCreateInfo view_ci{};
    view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_ci.image = swapchain_images_[i];
    view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_ci.format = surface_format_.format;
    view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CHECK_VK_RESULT(d().vkCreateImageView(device_, &view_ci, nullptr,
                                          &swapchain_image_views_[i]));
  }

  // --------------------------------------------------------------------------
  // Record a command buffer for each of the images to be executed prior to
  // presenting.
  // --------------------------------------------------------------------------

  present_transition_buffers_.resize(swapchain_images_.size());

  VkCommandBufferAllocateInfo buffers_info{};
  buffers_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  buffers_info.commandPool = swapchain_command_pool_;
  buffers_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  buffers_info.commandBufferCount =
      static_cast<uint32_t>(present_transition_buffers_.size());

  CHECK_VK_RESULT(d().vkAllocateCommandBuffers(
      device_, &buffers_info, present_transition_buffers_.data()));

  for (size_t i = 0; i < swapchain_images_.size(); i++) {
    auto image = swapchain_images_[i];
    auto buffer = present_transition_buffers_[i];

    VkCommandBufferBeginInfo begin_info{};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    CHECK_VK_RESULT(d().vkBeginCommandBuffer(buffer, &begin_info));

    // Filament Engine hands back the image after writing to it
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    d().vkCmdPipelineBarrier(buffer,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);

    CHECK_VK_RESULT(d().vkEndCommandBuffer(buffer));
  }

  // One present-transition semaphore per swapchain image (see header). Created
  // here, alongside the per-image transition command buffers, so the count
  // tracks the swapchain.
  VkSemaphoreCreateInfo present_sem_info{};
  present_sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  present_transition_semaphores_.resize(swapchain_images_.size());
  for (auto& sem : present_transition_semaphores_) {
    CHECK_VK_RESULT(
        d().vkCreateSemaphore(device_, &present_sem_info, nullptr, &sem));
  }

#if BUILD_COMPOSITOR
  CompositorPipeliningInit();
#endif

  return true;
}

VKAPI_ATTR VkBool32

    VKAPI_CALL
    WaylandVulkanBackend::debugReportCallback(
        const VkDebugReportFlagsEXT flags,
        const VkDebugReportObjectTypeEXT /* objectType */,
        const uint64_t /* object */,
        const size_t /* location */,
        const int32_t /* messageCode */,
        const char* pLayerPrefix,
        const char* pMessage,
        void* /* pUserData */) {
  if (flags & VK_DEBUG_REPORT_INFORMATION_BIT_EXT) {
    ihs::log::info("Vulkan Report: ({}) {}", pLayerPrefix, pMessage);
  } else if (flags & (VK_DEBUG_REPORT_WARNING_BIT_EXT |
                      VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT)) {
    ihs::log::warn("Vulkan Report: ({}) {}", pLayerPrefix, pMessage);
  } else if (flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) {
    ihs::log::error("Vulkan Report: ({}) {}", pLayerPrefix, pMessage);
  } else if (flags & VK_DEBUG_REPORT_DEBUG_BIT_EXT) {
    ihs::log::debug("Vulkan Report: ({}) {}", pLayerPrefix, pMessage);
  }
  return VK_FALSE;
}

VKAPI_ATTR VkBool32 VKAPI_CALL WaylandVulkanBackend::debugUtilsCallback(
    const VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    const VkDebugUtilsMessageTypeFlagsEXT /* types */,
    const VkDebugUtilsMessengerCallbackDataEXT* cb_data,
    void* /* pUserData */) {
  if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
    ihs::log::info("Vulkan Dbg: ({}) {}", cb_data->pMessageIdName,
                   cb_data->pMessage);
  } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
    ihs::log::error("Vulkan Dbg: ({}) {}", cb_data->pMessageIdName,
                    cb_data->pMessage);
  }
  return VK_TRUE;
}

FlutterVulkanImage WaylandVulkanBackend::GetNextImageCallback(
    void* user_data,
    const FlutterFrameInfo* frame_info) {
  if (frame_info->struct_size != sizeof(FlutterFrameInfo)) {
    ihs::log::error(
        "GetNextImageCallback: frame_info->struct_size != "
        "sizeof(FlutterFrameInfo)");
  }
  const auto state = static_cast<FlutterDesktopEngineState*>(user_data);
  const auto b = reinterpret_cast<WaylandVulkanBackend*>(
      state->view_controller->view->GetBackend());
  if (b->resize_pending_) {
    b->InitializeSwapChain();
  }

  CHECK_VK_RESULT(d().vkAcquireNextImageKHR(
      b->device_, b->swapchain_, 1'000'000'000,  // timeout (ns) 1000ms
      nullptr, b->image_ready_fence_, &b->last_image_index_));

  CHECK_VK_RESULT(d().vkWaitForFences(b->device_, 1, &b->image_ready_fence_,
                                      true, UINT64_MAX));
  CHECK_VK_RESULT(d().vkResetFences(b->device_, 1, &b->image_ready_fence_));

  return {
      .struct_size = sizeof(FlutterVulkanImage),
      .image = reinterpret_cast<uint64_t>(
          b->swapchain_images_[b->last_image_index_]),
      .format = static_cast<uint32_t>(b->surface_format_.format),
  };
}

bool WaylandVulkanBackend::PresentCallback(
    void* user_data,
    const FlutterVulkanImage* /* image */) {
  const auto state = static_cast<FlutterDesktopEngineState*>(user_data);
  const auto b = reinterpret_cast<WaylandVulkanBackend*>(
      state->view_controller->view->GetBackend());

  // Ensure the layout transition happens after the render pass in the same
  // command buffer Record vkCmdPipelineBarrier at the end of your render pass
  // command buffer

  // Submit the command buffer and signal this image's present-transition
  // semaphore. Both the command buffer and the semaphore are indexed by
  // image, so the next frame is free to record/submit while this frame's
  // GPU work is still in flight — no full-queue drain is required (the
  // resource is only reused once vkAcquireNextImageKHR hands this image
  // back, which already implies its previous presentation completed).
  VkSemaphore& transition_semaphore =
      b->present_transition_semaphores_[b->last_image_index_];
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers =
      &b->present_transition_buffers_[b->last_image_index_];
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &transition_semaphore;
  {
    std::lock_guard<std::mutex> queue_lock(b->queue_mutex_);
    d().vkQueueSubmit(b->queue_, 1, &submit_info, nullptr);
  }

  // Wait on the signaled semaphore in vkQueuePresentKHR
  VkPresentInfoKHR present_info{};
  present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present_info.waitSemaphoreCount = 1;
  present_info.pWaitSemaphores = &transition_semaphore;
  present_info.swapchainCount = 1;
  present_info.pSwapchains = &b->swapchain_;
  present_info.pImageIndices = &b->last_image_index_;
  // Mint the wp_presentation_feedback BEFORE vkQueuePresentKHR — Mesa's
  // Wayland WSI does wl_surface.commit inside that call, and the
  // feedback object binds to the most recent surface request prior to
  // the commit.
  b->RequestPresentationFeedback();
  VkResult result;
  {
    std::lock_guard<std::mutex> queue_lock(b->queue_mutex_);
    result = d().vkQueuePresentKHR(b->queue_, &present_info);
  }

  // If the swap chain is no longer compatible with the surface, defer
  // recreation to the next GetNextImageCallback rather than rebuilding inside
  // the present call. That path (gated on resize_pending_) idles the queue and
  // destroys the old swapchain, command buffers and per-image semaphores
  // before recreating them; rebuilding here would skip that teardown and leak
  // them. Mirrors the compositor path (PresentLayersImpl).
  if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
    b->resize_pending_ = true;
  }
  // No vkQueueWaitIdle here: CPU/GPU now pipeline across frames. Reuse of
  // this image's command buffer and semaphore is gated by the next
  // vkAcquireNextImageKHR + image_ready_fence_ wait in GetNextImageCallback,
  // and swapchain recreation (above / on resize) drains the queue itself.

  b->ProfilePresent(result == VK_SUCCESS);

  return result == VK_SUCCESS;
}

void* WaylandVulkanBackend::GetInstanceProcAddressCallback(
    void* /* user_data */,
    FlutterVulkanInstanceHandle instance,
    const char* procname) {
  auto* vk_instance = static_cast<VkInstance>(instance);
  // Per the embedder API contract (embedder.h,
  // get_instance_proc_address_callback), swap out vkQueue* entry points for
  // locking variants: the engine's raster thread and this backend's
  // present/transfer paths share the single graphics queue, and host access
  // to a VkQueue must be externally synchronized. Also shims
  // vkGetDeviceProcAddr so device-level resolution (Skia VulkanProcTable
  // path) is covered. See issue #208.
  if (auto* interposed = wayland_vulkan::QueueInterposer::Interpose(
          vk_instance, procname, d().vkGetInstanceProcAddr)) {
    return reinterpret_cast<void*>(interposed);
  }
  auto* proc = d().vkGetInstanceProcAddr(vk_instance, procname);
  return reinterpret_cast<void*>(proc);
}

void WaylandVulkanBackend::ProfilePresent(const bool ok) {
  // Single env-var probe per process; the lookup is O(strlen) and we
  // call this on every frame.
  static const bool profile_enabled =
      profiling::FrameProfile::Enabled("IVI_VK_PROFILE");
  if (!profile_enabled) {
    return;
  }
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
                          static_cast<uint64_t>(ts.tv_nsec);

  auto& p = profile_;
  if (!ok) {
    ++p.present_failures;
    return;
  }
  if (p.last_present_ns != 0) {
    const uint64_t dt = now_ns - p.last_present_ns;
    p.interval_sum_ns += dt;
    if (dt > p.interval_max_ns) {
      p.interval_max_ns = dt;
    }
    // Same bucket thresholds as wayland_egl so histograms line up.
    if (dt <= 17'000'000ULL) {
      ++p.bucket_60hz;
    } else if (dt <= 33'000'000ULL) {
      ++p.bucket_30hz;
    } else if (dt <= 50'000'000ULL) {
      ++p.bucket_20hz;
    } else if (dt <= 100'000'000ULL) {
      ++p.bucket_slow;
    } else {
      ++p.bucket_idle;
    }
  }
  p.last_present_ns = now_ns;
  ++p.presented_frames;

  // beginFrame-to-present latency. last_begin_frame_ns_ is 0 when
  // vsync_callback isn't wired; in that case we have no Flutter-scheduled
  // start time to compare against, so skip. Pipelining (multiple frames
  // in flight) means a given present may not correspond to the *most
  // recent* OnVsync — the running mean still tracks the difference
  // between vsync-aware and wall-clock scheduling under load.
  const uint64_t begin = last_begin_frame_ns_.load(std::memory_order_acquire);
  if (begin > 0 && now_ns > begin) {
    const uint64_t latency = now_ns - begin;
    p.pipeline_latency_sum_ns += latency;
    if (latency > p.pipeline_latency_max_ns) {
      p.pipeline_latency_max_ns = latency;
    }
    ++p.pipeline_latency_samples;
  }

  constexpr uint32_t kProfileWindow = 60;
  if (p.presented_frames < kProfileWindow) {
    return;
  }
  const uint32_t samples =
      (p.interval_sum_ns > 0) ? p.presented_frames - 1 : p.presented_frames;
  const uint64_t mean_ns = samples > 0 ? p.interval_sum_ns / samples : 0;
  const double fps = mean_ns > 0 ? 1e9 / static_cast<double>(mean_ns) : 0.0;
  const uint64_t lat_mean_us =
      p.pipeline_latency_samples > 0
          ? (p.pipeline_latency_sum_ns / p.pipeline_latency_samples) / 1000
          : 0;
  ihs::log::info(
      "[WaylandVulkanBackend] profile (n={}): fps={:.2f} mean_interval={}us "
      "max_interval={}us present_failures={} discarded={} "
      "refresh={}us flags=0x{:x} pipeline_mean={}us pipeline_max={}us "
      "buckets[60Hz/30Hz/20Hz/slow/idle]={}/{}/{}/{}/{}",
      p.presented_frames, fps, mean_ns / 1000, p.interval_max_ns / 1000,
      p.present_failures, p.discarded_frames,
      // refresh period now lives in WaylandVsyncProvider; 0 = "not tracked
      // here"
      0, p.flags_or, lat_mean_us, p.pipeline_latency_max_ns / 1000,
      p.bucket_60hz, p.bucket_30hz, p.bucket_20hz, p.bucket_slow,
      p.bucket_idle);

  auto& s = session_totals_;
  s.presented_frames += p.presented_frames;
  s.present_failures += p.present_failures;
  s.discarded_frames += p.discarded_frames;
  s.flags_or |= p.flags_or;
  s.interval_sum_ns += p.interval_sum_ns;
  if (p.interval_max_ns > s.interval_max_ns) {
    s.interval_max_ns = p.interval_max_ns;
  }
  s.bucket_60hz += p.bucket_60hz;
  s.bucket_30hz += p.bucket_30hz;
  s.bucket_20hz += p.bucket_20hz;
  s.bucket_slow += p.bucket_slow;
  s.bucket_idle += p.bucket_idle;
  s.pipeline_latency_sum_ns += p.pipeline_latency_sum_ns;
  s.pipeline_latency_samples += p.pipeline_latency_samples;
  if (p.pipeline_latency_max_ns > s.pipeline_latency_max_ns) {
    s.pipeline_latency_max_ns = p.pipeline_latency_max_ns;
  }
  p = FrameProfile{.last_present_ns = p.last_present_ns};
}

// -----------------------------------------------------------------------------
// Debug HUD (Dear ImGui). Created on the raster thread on first present when
// IVI_HUD is set (or on the first ToggleHud), recorded into the present command
// buffer, and fed input from the platform thread via the shell.
// -----------------------------------------------------------------------------

#if BUILD_HUD
bool WaylandVulkanBackend::MaybeRenderHud(VkCommandBuffer cmd,
                                          VkImage image,
                                          VkImageView view,
                                          uint32_t w,
                                          uint32_t h,
                                          VkImageLayout old_layout,
                                          const FlutterLayer** layers,
                                          size_t count) {
  if (!hud_checked_) {
    hud_checked_ = true;
    hud_enabled_ = std::getenv("IVI_HUD") != nullptr || hud_config_enable_;
  }
  if (!hud_enabled_ || hud_init_failed_) {
    return false;
  }
  if (!hud_) {
    std::string err;
    // Match the HUD render pass to the actual target image format. The dma-buf
    // ring images are B8G8R8A8_UNORM and the swapchain-format selection is
    // skipped on that path (surface_format_ stays UNDEFINED), so pick the ring
    // format explicitly there; the WSI path uses the chosen swapchain format.
    const VkFormat hud_format = dmabuf_present_active_
                                    ? VK_FORMAT_B8G8R8A8_UNORM
                                    : surface_format_.format;
    const uint32_t hud_image_count =
        dmabuf_present_active_
            ? static_cast<uint32_t>(dmabuf_slots_.size())
            : static_cast<uint32_t>(swapchain_images_.size());
    hud_ = ihs::hud::VulkanHud::Create(
        instance_, physical_device_, device_, queue_family_index_, queue_,
        reinterpret_cast<void*>(d().vkGetInstanceProcAddr), hud_format,
        hud_image_count, err);
    if (!hud_) {
      hud_init_failed_ = true;
      ihs::log::warn("[WaylandVulkanBackend] HUD unavailable ({})", err);
      return false;
    }
    hud_->SetConfig(hud_config_);  // appearance from [hud] config
    hud_->SetOpen(true);           // config/IVI_HUD summons it open
    ihs::log::info("[WaylandVulkanBackend] debug HUD enabled");
  }

  // Present-interval window for the HUD's fps/frame stats (independent of
  // IVI_VK_PROFILE). Raster thread is the only writer.
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now_ns = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
                          static_cast<uint64_t>(ts.tv_nsec);
  float dt_s = 1.0f / 60.0f;
  if (hud_last_present_ns_ != 0 && now_ns > hud_last_present_ns_) {
    const uint64_t dt = now_ns - hud_last_present_ns_;
    dt_s = static_cast<float>(dt) / 1e9f;
    hud_interval_ms_[hud_interval_head_] = static_cast<float>(dt) / 1e6f;
    hud_interval_head_ = (hud_interval_head_ + 1) % hud_interval_ms_.size();
    if (hud_interval_count_ < hud_interval_ms_.size()) {
      ++hud_interval_count_;
    }
  }
  hud_last_present_ns_ = now_ns;

  ihs::hud::HudStats stats{};
  if (hud_interval_count_ > 0) {
    float sum = 0.0f;
    float worst = 0.0f;
    for (uint32_t i = 0; i < hud_interval_count_; ++i) {
      sum += hud_interval_ms_[i];
      worst = std::max(worst, hud_interval_ms_[i]);
    }
    stats.frame_ms = sum / static_cast<float>(hud_interval_count_);
    stats.frame_max_ms = worst;
    stats.fps = stats.frame_ms > 0.0f ? 1000.0f / stats.frame_ms : 0.0f;
  }
  stats.dmabuf_present = dmabuf_present_active_;
  stats.explicit_sync = explicit_sync_ != nullptr;
  const uint32_t refresh_ns = vsync_ != nullptr ? vsync_->RefreshPeriodNs() : 0;
  stats.target_fps =
      refresh_ns > 0 ? 1e9f / static_cast<float>(refresh_ns) : 60.0f;

  // Snapshot the platform views composited this frame so the HUD can track
  // per-view present counts and flash add/dispose.
  std::vector<ihs::hud::HudViewSample> views;
  for (size_t i = 0; i < count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (layer != nullptr &&
        layer->type == kFlutterLayerContentTypePlatformView &&
        layer->platform_view != nullptr) {
      views.push_back({layer->platform_view->identifier,
                       static_cast<uint32_t>(layer->size.width),
                       static_cast<uint32_t>(layer->size.height),
                       dmabuf_present_active_});
    }
  }

  // Bring the swapchain image to GENERAL for the HUD's LOAD render pass.
  if (old_layout != VK_IMAGE_LAYOUT_GENERAL) {
    VkImageMemoryBarrier to_general{};
    to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_general.oldLayout = old_layout;
    to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.image = image;
    to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_general.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &to_general);
  }

  hud_->Render(cmd, view, w, h, dt_s, stats, views);
  return true;
}
#endif  // BUILD_HUD

// -----------------------------------------------------------------------------
// wp_presentation-driven vsync. Mirrors WaylandEglBackend; the only path
// differences are (a) the present-path hook fires before vkQueuePresentKHR
// instead of eglSwapBuffers, and (b) the Vulkan backend has no
// `clock_compatible_` shortcut in the present path because there's
// nothing equivalent to EGL's MakeCurrent/dispatch interleaving.
// -----------------------------------------------------------------------------

VsyncCallback WaylandVulkanBackend::GetVsyncCallback() const {
  if (vsync_ == nullptr || !vsync_->Usable()) {
    return nullptr;
  }
  return &VsyncTrampoline;
}

void WaylandVulkanBackend::VsyncTrampoline(void* user_data,
                                           const intptr_t baton) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr || state->view_controller == nullptr ||
      state->view_controller->engine == nullptr) {
    return;
  }
  auto* engine_obj = state->view_controller->engine;
  auto* backend = dynamic_cast<WaylandVulkanBackend*>(engine_obj->GetBackend());
  if (backend == nullptr) {
    return;
  }
  backend->SetVsyncBaton(engine_obj->GetFlutterEngine(), baton);
}

void WaylandVulkanBackend::SetVsyncBaton(FLUTTER_API_SYMBOL(FlutterEngine)
                                             engine,
                                         const intptr_t baton) {
  if (vsync_ != nullptr) {
    vsync_->SubmitBaton(engine, baton);
  }
}

void WaylandVulkanBackend::RequestPresentationFeedback() {
  // Rasterizer thread, BEFORE the vkQueuePresentKHR that mints the
  // wl_surface.commit the feedback binds to. The provider no-ops when
  // wp_presentation isn't usable or no surface is attached yet.
  if (vsync_ != nullptr) {
    vsync_->RequestFeedback();
  }
}

void WaylandVulkanBackend::StopVsyncMonitor() {
  if (vsync_ != nullptr) {
    vsync_->Stop();
  }
}

void WaylandVulkanBackend::Resize(size_t /* index */,
                                  Engine* engine,
                                  const int32_t width,
                                  const int32_t height) {
  if (width_ != static_cast<uint32_t>(width) ||
      height_ != static_cast<uint32_t>(height)) {
    resize_pending_ = true;
    width_ = static_cast<uint32_t>(width);
    height_ = static_cast<uint32_t>(height);
    if (engine) {
      if (engine->SetWindowSize(static_cast<size_t>(height),
                                static_cast<size_t>(width)) != kSuccess) {
        ihs::log::error("Failed to set Flutter Engine Window Size");
      }
    }
  }
}

void WaylandVulkanBackend::OnDmabufFeedback(const wl::FeedbackSnapshot& snap) {
  {
    std::lock_guard<std::mutex> lock(fb_mu_);
    fb_snapshot_ = snap;
  }
  const auto scanout = snap.ScanoutModifiersFor(DRM_FORMAT_XRGB8888);
  const auto all = snap.ModifiersFor(DRM_FORMAT_XRGB8888);
  ihs::log::info(
      "[WaylandVulkanBackend] dma-buf feedback: main_device={:#x} "
      "XRGB8888 modifiers scanout={} total={}",
      static_cast<uint64_t>(snap.main_device), scanout.size(), all.size());
  for (const uint64_t m : scanout) {
    ihs::log::debug("[WaylandVulkanBackend]   scanout modifier {:#018x}", m);
  }
}

void WaylandVulkanBackend::CreateSurface(size_t /* index */,
                                         wl_surface* surface,
                                         int32_t /* width */,
                                         int32_t /* height */) {
  IHS_DEBUG("CreateSurface");
  assert(instance_ != VK_NULL_HANDLE);
  assert(surface_ == VK_NULL_HANDLE);
  assert(wl_display_ != nullptr);
  assert(surface != nullptr);

  surface_ = VK_NULL_HANDLE;

  // Stash for wp_presentation_feedback. Released-acquire so the
  // rasterizer-thread RequestPresentationFeedback path sees it the next
  // time it's called after CreateSurface (which always runs on the
  // platform thread before any frames present).
  wl_surface_.store(surface, std::memory_order_release);
  if (vsync_ != nullptr) {
    vsync_->SetSurface(surface);
  }

  // Bind v4 dma-buf feedback for the zero-copy present path. Independent of
  // Mesa's WSI dmabuf bind; per-surface feedback carries the accurate scanout
  // flag for this surface. A roundtrip drains the initial advertisement so
  // fb_snapshot_ is populated before the first frame.
  if (shell_display_ != nullptr && shell_display_->GetLinuxDmabufName() != 0) {
    dmabuf_feedback_ =
        std::make_unique<wl::DmabufFeedback<WaylandVulkanBackend>>();
    dmabuf_feedback_->Record(shell_display_->GetLinuxDmabufName(),
                             shell_display_->GetLinuxDmabufVersion());
    if (dmabuf_feedback_->Bind(shell_display_->GetRegistry(), this) &&
        dmabuf_feedback_->StartSurface(wl_display_,
                                       reinterpret_cast<wl_proxy*>(surface))) {
      wl_display_roundtrip(wl_display_);
    } else {
      ihs::log::info(
          "[WaylandVulkanBackend] dma-buf feedback v{} unavailable; zero-copy "
          "present path disabled (WSI swapchain fallback)",
          shell_display_->GetLinuxDmabufVersion());
      dmabuf_feedback_.reset();
    }
  }

  VkWaylandSurfaceCreateInfoKHR createInfo{};
  createInfo.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
  createInfo.display = wl_display_;
  createInfo.surface = surface;

  CHECK_VK_RESULT(d().vkCreateWaylandSurfaceKHR(instance_, &createInfo, nullptr,
                                                &surface_));

  findPhysicalDevice();
  createLogicalDevice();

#if BUILD_COMPOSITOR
  // Select the zero-copy dma-buf present path when the compositor advertises a
  // modifier this device can render into and IVI_VK_DMABUF is set. Prefer
  // scanout-tranche modifiers (the compositor's plane-promotable candidates),
  // fall back to any advertised one. Off by default: the swapchain path stays
  // the default until this proves out.
  if (dmabuf_feedback_) {
    std::vector<uint64_t> comp_mods;
    {
      std::lock_guard<std::mutex> lock(fb_mu_);
      comp_mods = fb_snapshot_.ScanoutModifiersFor(DRM_FORMAT_XRGB8888);
      if (comp_mods.empty()) {
        comp_mods = fb_snapshot_.ModifiersFor(DRM_FORMAT_XRGB8888);
      }
    }
    dmabuf_modifiers_ = wl_vulkan::NegotiateModifiers(
        physical_device_, VK_FORMAT_B8G8R8A8_UNORM, comp_mods);
    const bool env_on = std::getenv("IVI_VK_DMABUF") != nullptr;
    if (!dmabuf_modifiers_.empty() && env_on) {
      dmabuf_present_active_ = true;
      ihs::log::info(
          "[WaylandVulkanBackend] zero-copy dma-buf present ENABLED ({} "
          "renderable modifier(s) shared with the compositor)",
          dmabuf_modifiers_.size());
      // Bring up the alpha-blend layer compositor for correct src-over
      // composition (platform views under transparent overlays). Falls back to
      // overwrite blits if it can't be created.
      {
        std::string comp_err;
        layer_compositor_ = wl_vulkan::LayerCompositor::Create(
            device_, VK_FORMAT_B8G8R8A8_UNORM, comp_err);
        if (!layer_compositor_) {
          ihs::log::warn(
              "[WaylandVulkanBackend] layer compositor unavailable ({}); "
              "falling back to overwrite blits (overlapping layers may not "
              "blend)",
              comp_err);
        }
      }
      // Bring up wp_linux_drm_syncobj explicit sync if the compositor
      // advertises the manager and the device supports the timeline bridge. On
      // failure the CPU fence is used — never fatal. IVI_VK_NO_EXPLICIT_SYNC
      // forces the CPU fence (bisecting / A-B against the timeline path).
      if (shell_display_ != nullptr &&
          shell_display_->GetDrmSyncobjName() != 0 &&
          std::getenv("IVI_VK_NO_EXPLICIT_SYNC") == nullptr) {
        std::string sync_err;
        explicit_sync_ = wl_vulkan::ExplicitSync::Create(
            physical_device_, device_, shell_display_->GetRegistry(),
            shell_display_->GetDrmSyncobjName(),
            shell_display_->GetDrmSyncobjVersion(), wl_display_,
            reinterpret_cast<wl_proxy*>(surface), sync_err);
        if (!explicit_sync_) {
          ihs::log::info(
              "[WaylandVulkanBackend] explicit sync unavailable ({}); using "
              "CPU "
              "fence for the dma-buf path",
              sync_err);
        }
      }
    } else {
      ihs::log::info(
          "[WaylandVulkanBackend] dma-buf present {} (modifiers={}, env={}); "
          "using WSI swapchain",
          dmabuf_modifiers_.empty() ? "unavailable" : "disabled",
          dmabuf_modifiers_.size(), env_on);
    }
  }
#endif

  // --------------------------------------------------------------------------
  // Create sync primitives and command pool to use in the render loop
  // callbacks.
  // --------------------------------------------------------------------------

  // swapchain_command_pool_ backs TransitionLayout, which runs on BOTH present
  // paths (backing-store layout transitions in CreateBackingStore), so always
  // create it — the name is historical.
  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.queueFamilyIndex = queue_family_index_;
  d().vkCreateCommandPool(device_, &pool_info, nullptr,
                          &swapchain_command_pool_);

  // The VkSwapchainKHR (and its image-acquire fence) is only for the WSI
  // present path. On the dma-buf path we commit buffers directly on the
  // wl_surface and never create a swapchain — a Mesa WSI swapchain on the same
  // surface would otherwise own the surface's buffer state. skia-vulkan-dmabuf
  // works the same.
  if (!dmabuf_present_active_) {
    VkFenceCreateInfo f_info{};
    f_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    d().vkCreateFence(device_, &f_info, nullptr, &image_ready_fence_);

    // present_transition_semaphores_ are created per swapchain image inside
    // InitializeSwapChain (below), since their count tracks the swapchain.
    if (!InitializeSwapChain()) {
      ihs::log::critical("Failed to create swap chain.");
      exit(EXIT_FAILURE);
    }
  }
}

bool WaylandVulkanBackend::CollectBackingStore(const FlutterBackingStore* store,
                                               void* user_data) {
#if BUILD_COMPOSITOR
  // user_data is `this` per FlutterCompositor.user_data set in
  // GetCompositorConfig(). The prior code treated it as a
  // FlutterDesktopEngineState* and walked view_controller->view->GetBackend
  // — that read garbage offsets off the backend instance and SEGV'd at
  // first use, leaving the Vulkan backend non-functional.
  auto* b = static_cast<WaylandVulkanBackend*>(user_data);
  return b->CollectBackingStoreImpl(store);
#else
  (void)store;
  (void)user_data;
  IHS_DEBUG("CollectBackingStore");
  return false;
#endif
}

bool WaylandVulkanBackend::CreateBackingStore(
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* backing_store_out,
    void* user_data) {
#if BUILD_COMPOSITOR
  // user_data is `this` per FlutterCompositor.user_data — see
  // CollectBackingStore above for the prior-bug context.
  auto* b = static_cast<WaylandVulkanBackend*>(user_data);
  return b->CreateBackingStoreImpl(config, backing_store_out);
#else
  (void)config;
  (void)backing_store_out;
  (void)user_data;
  IHS_DEBUG("CreateBackingStore");
#if 0  /// TODO
    auto surface_size = SkISize::Make(config->size.width, config->size.height);
    TestVulkanImage* test_image = new TestVulkanImage(
        std::move(test_vulkan_context_->CreateImage(surface_size).value()));

    GrVkImageInfo image_info{};
    image_info.fImage = test_image->GetImage();
    image_info.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.fImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.fFormat = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.fImageUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.fSampleCount = 1;
    image_info.fLevelCount = 1;

    GrBackendTexture backend_texture(surface_size.width(), surface_size.height(),
                                     image_info);

    SkSurfaceProps surface_properties(0, kUnknown_SkPixelGeometry);

    SkSurface::TextureReleaseProc release_vktexture = [](void* user_data) {
      delete reinterpret_cast<TestVulkanImage*>(user_data);
    };

    sk_sp<SkSurface> surface = SkSurface::MakeFromBackendTexture(
        context_.get(),            // context
        backend_texture,           // back-end texture
        kTopLeft_GrSurfaceOrigin,  // surface origin
        1,                         // sample count
        kRGBA_8888_SkColorType,    // color type
        SkColorSpace::MakeSRGB(),  // color space
        &surface_properties,       // surface properties
        release_vktexture,         // texture release proc
        test_image                 // release context
    );

    if (!surface) {
      ihs::log::error("Could not create Skia surface from Vulkan image.");
      return false;
    }
    backing_store_out->type = kFlutterBackingStoreTypeVulkan;

    auto* image = new FlutterVulkanImage();
    image->image = reinterpret_cast<uint64_t>(image_info.fImage);
    image->format = VK_FORMAT_R8G8B8A8_UNORM;
    backing_store_out->vulkan.image = image;

    // Collect all allocated resources in the destruction_callback.
    {
      UserData* user_data = new UserData();
      user_data->image = image;
      user_data->surface = surface.get();

      backing_store_out->user_data = user_data;
      backing_store_out->vulkan.user_data = user_data;
      backing_store_out->vulkan.destruction_callback = [](void* user_data) {
        UserData* d = reinterpret_cast<UserData*>(user_data);
        d->surface->unref();
        delete d->image;
        delete d;
      };

      // The balancing unref is in the destruction callback.
      surface->ref();
    }

    return true;
#endif
  return false;
#endif  // BUILD_COMPOSITOR
}

bool WaylandVulkanBackend::PresentLayers(const FlutterLayer** layers,
                                         size_t layers_count,
                                         void* user_data) {
#if BUILD_COMPOSITOR
  // user_data is `this` per FlutterCompositor.user_data — see
  // CollectBackingStore above for the prior-bug context.
  auto* b = static_cast<WaylandVulkanBackend*>(user_data);
  return b->PresentLayersImpl(layers, layers_count);
#else
  (void)layers;
  (void)layers_count;
  (void)user_data;
  IHS_DEBUG("PresentLayers");
  return false;
#endif
}

bool WaylandVulkanBackend::TextureMakeCurrent() {
  return true;
}

bool WaylandVulkanBackend::TextureClearCurrent() {
  return true;
}

#if BUILD_COMPOSITOR

namespace {
struct VulkanStoreBaton {
  WaylandVulkanBackend* backend;
  VulkanBackingStore* store;
};
}  // namespace

void WaylandVulkanBackend::RegisterCompositorSurface(
    FlutterPlatformViewIdentifier id,
    std::shared_ptr<ICompositorSurface> surface) {
  std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
  m_compositor_surfaces[id] = std::move(surface);
  if (ivi::pv_latency::Enabled()) {
    ihs::log::info("[pv-latency] surface registered id={} live={}", id,
                   m_compositor_surfaces.size());
  }
}

void WaylandVulkanBackend::UnregisterCompositorSurface(
    FlutterPlatformViewIdentifier id) {
  std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
  m_compositor_surfaces.erase(id);
  if (ivi::pv_latency::Enabled()) {
    ihs::log::info("[pv-latency] surface disposed id={} live={}", id,
                   m_compositor_surfaces.size());
  }
}

void WaylandVulkanBackend::ResizeCompositorSurface(
    FlutterPlatformViewIdentifier id,
    int32_t width,
    int32_t height) {
  std::shared_ptr<ICompositorSurface> surface;
  {
    std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
    const auto it = m_compositor_surfaces.find(id);
    if (it == m_compositor_surfaces.end()) {
      return;
    }
    surface = it->second;
  }
  if (surface) {
    surface->OnResize(width, height);
  }
}

bool WaylandVulkanBackend::CreateBackingStoreImpl(
    const FlutterBackingStoreConfig* config,
    FlutterBackingStore* store_out) {
  const auto w = static_cast<int32_t>(config->size.width);
  const auto h = static_cast<int32_t>(config->size.height);

  const bool want_dma_buf = BUILD_COMPOSITOR_DMABUF_EXPORT != 0;
  auto store =
      m_store_pool.Acquire(w, h, device_, physical_device_, want_dma_buf);
  if (!store->IsValid()) {
    ihs::log::error("WaylandVulkanBackend: failed to create backing store");
    return false;
  }
  // Record whether export succeeded at least once (for introspection).
  if (store->has_dma_buf()) {
    dma_buf_export_ok_ = true;
  }

  // Engine renders with the image in COLOR_ATTACHMENT_OPTIMAL; transition
  // UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL before handing it over.
  if (store->Layout() == VK_IMAGE_LAYOUT_UNDEFINED) {
    TransitionLayout(store->Image(), VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    store->SetLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  }

  auto* baton = new VulkanStoreBaton{this, store.get()};

  store_out->struct_size = sizeof(FlutterBackingStore);
  store_out->type = kFlutterBackingStoreTypeVulkan;
  store_out->user_data = baton;
  store_out->vulkan.struct_size = sizeof(FlutterVulkanBackingStore);
  store_out->vulkan.image = store->engine_image();
  store_out->vulkan.user_data = baton;
  store_out->vulkan.destruction_callback = [](void*) {
    // Store ownership lives in m_alive_stores; Collect does the cleanup.
  };

  VulkanBackingStore* key = store.get();
  m_alive_stores[key] = std::move(store);
  return true;
}

bool WaylandVulkanBackend::CollectBackingStoreImpl(
    const FlutterBackingStore* store) {
  auto* baton = static_cast<VulkanStoreBaton*>(store->user_data);
  if (!baton) {
    return false;
  }
  const auto it = m_alive_stores.find(baton->store);
  if (it != m_alive_stores.end()) {
    m_store_pool.Release(std::move(it->second));
    m_alive_stores.erase(it);
  }
  delete baton;
  return true;
}

namespace {
// Map a (single) pipeline stage to the image-access flags that stage is
// allowed to perform, so a barrier's access masks always satisfy
// VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02819/02820. Covers the
// stages used by TransitionLayout's callers; TOP/BOTTOM_OF_PIPE carry no
// access.
VkAccessFlags AccessMaskForStage(VkPipelineStageFlags stage) {
  switch (stage) {
    case VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT:
      return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_PIPELINE_STAGE_TRANSFER_BIT:
      return VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    case VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:
    case VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:
    default:
      return 0;
  }
}
}  // namespace

void WaylandVulkanBackend::TransitionLayout(
    VkImage image,
    VkImageLayout from,
    VkImageLayout to,
    VkPipelineStageFlags src_stage,
    VkPipelineStageFlags dst_stage) const {
  VkCommandBufferAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc.commandPool = swapchain_command_pool_;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd{};
  if (d().vkAllocateCommandBuffers(device_, &alloc, &cmd) != VK_SUCCESS) {
    ihs::log::error("TransitionLayout: vkAllocateCommandBuffers failed");
    return;
  }

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  d().vkBeginCommandBuffer(cmd, &begin);

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.layerCount = 1;
  // Derive access masks from the stage masks so they stay spec-valid
  // (VUID-vkCmdPipelineBarrier-pImageMemoryBarriers-02819/02820): an access
  // flag must be supported by its stage. The previous hardcoded
  // COLOR_ATTACHMENT_WRITE|TRANSFER_WRITE / *_READ|*_WRITE masks were invalid
  // for the only caller's TOP_OF_PIPE -> COLOR_ATTACHMENT_OUTPUT transition.
  barrier.srcAccessMask = AccessMaskForStage(src_stage);
  barrier.dstAccessMask = AccessMaskForStage(dst_stage);

  d().vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr,
                           1, &barrier);
  d().vkEndCommandBuffer(cmd);

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  {
    std::lock_guard<std::mutex> queue_lock(queue_mutex_);
    d().vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    d().vkQueueWaitIdle(queue_);
  }
  d().vkFreeCommandBuffers(device_, swapchain_command_pool_, 1, &cmd);
}

void WaylandVulkanBackend::BlitStoreToSwapchain(VkCommandBuffer cmd,
                                                VulkanBackingStore& src,
                                                VkImage dst,
                                                int32_t dst_x,
                                                int32_t dst_y,
                                                int32_t dst_w,
                                                int32_t dst_h) {
  // Transition source -> TRANSFER_SRC_OPTIMAL.
  VkImageMemoryBarrier src_to_xfer{};
  src_to_xfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  src_to_xfer.oldLayout = src.Layout();
  src_to_xfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  src_to_xfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  src_to_xfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  src_to_xfer.image = src.Image();
  src_to_xfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  src_to_xfer.subresourceRange.levelCount = 1;
  src_to_xfer.subresourceRange.layerCount = 1;
  src_to_xfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  src_to_xfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &src_to_xfer);

  VkImageBlit region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffsets[0] = {0, 0, 0};
  region.srcOffsets[1] = {src.Width(), src.Height(), 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstOffsets[0] = {dst_x, dst_y, 0};
  region.dstOffsets[1] = {dst_x + dst_w, dst_y + dst_h, 1};
  d().vkCmdBlitImage(cmd, src.Image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                     VK_FILTER_LINEAR);

  // Transition source back to COLOR_ATTACHMENT_OPTIMAL for the next frame.
  VkImageMemoryBarrier xfer_to_color = src_to_xfer;
  xfer_to_color.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  xfer_to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  xfer_to_color.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  xfer_to_color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                           nullptr, 0, nullptr, 1, &xfer_to_color);
  src.SetLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

bool WaylandVulkanBackend::PresentLayersImpl(const FlutterLayer** layers,
                                             size_t count) {
  if (dmabuf_present_active_) {
    return PresentLayersDmabuf(layers, count);
  }
  if (resize_pending_) {
    InitializeSwapChain();
  }
  if (m_compositor_slots_.empty()) {
    CompositorPipeliningInit();
  }

  // Pick the slot for this frame and wait until its previous use has
  // completed on the GPU. This is the only CPU/GPU sync — the queue is
  // never asked to idle.
  const FrameSlot& slot = m_compositor_slots_[m_compositor_current_frame_ %
                                              m_compositor_slots_.size()];
  CHECK_VK_RESULT(
      d().vkWaitForFences(device_, 1, &slot.in_flight, VK_TRUE, UINT64_MAX));
  CHECK_VK_RESULT(d().vkResetFences(device_, 1, &slot.in_flight));

  uint32_t image_index = 0;
  CHECK_VK_RESULT(d().vkAcquireNextImageKHR(device_, swapchain_, 1'000'000'000,
                                            slot.image_available,
                                            VK_NULL_HANDLE, &image_index));
  last_image_index_ = image_index;

  VkImage dst = swapchain_images_[image_index];

  VkCommandBuffer cmd = slot.cmd_buffer;
  d().vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  d().vkBeginCommandBuffer(cmd, &begin);

  // Explicit-sync acquire bookkeeping: retire semaphores from completed frames,
  // start a fresh per-frame wait list (filled by CollectAcquireWait during
  // compositing, consumed by the submit below).
  ReapAcquireWaits();
  ReapDeferredDestroys();
  ++acquire_wait_seq_;
  frame_acquire_waits_.clear();
  frame_pv_surfaces_.clear();

  // A Vulkan platform view only composites correctly with src-over alpha (a
  // transparent Flutter overlay must blend over it, not overwrite it to black).
  // When one is present, blend the whole layer stack into the swapchain via
  // layer_compositor_; otherwise keep the proven overwrite-blit path. The
  // compositor is created lazily here (the dma-buf path makes its own).
  bool has_platform_view = false;
  for (size_t i = 0; i < count; ++i) {
    if (layers[i] && layers[i]->type == kFlutterLayerContentTypePlatformView &&
        layers[i]->platform_view) {
      has_platform_view = true;
      break;
    }
  }
  if (has_platform_view && !layer_compositor_) {
    std::string comp_err;
    layer_compositor_ = wl_vulkan::LayerCompositor::Create(
        device_, surface_format_.format, comp_err);
    if (!layer_compositor_) {
      ihs::log::warn(
          "[WaylandVulkanBackend] layer compositor unavailable ({}); platform "
          "view falls back to overwrite blit (a transparent overlay may erase "
          "it)",
          comp_err);
    }
  }
  const bool blend = has_platform_view && layer_compositor_;

  // Sequence platform-view subsurface Z-order for this frame.
  m_sequencer.Present(layers, count, nullptr,
                      [this](FlutterPlatformViewIdentifier id) {
                        WarnMissingPlatformView(id);
                      });

  bool ok = true;
  if (blend) {
    // Alpha-blend the whole stack (backing stores + platform views) straight
    // into the swapchain image. The render pass clears then leaves it in
    // GENERAL; transition that to PRESENT_SRC below.
    // Size the blend to the swapchain images' actual extent (not
    // width_/height_, which can differ mid-resize) so the framebuffer matches
    // the image views.
    ok = CompositeLayersBlend(cmd, layers, count,
                              swapchain_image_views_[image_index],
                              swapchain_extent_.width, swapchain_extent_.height,
                              m_compositor_current_frame_);

#if BUILD_HUD
    // Image is already in GENERAL; the HUD leaves it there.
    MaybeRenderHud(cmd, dst, swapchain_image_views_[image_index],
                   swapchain_extent_.width, swapchain_extent_.height,
                   VK_IMAGE_LAYOUT_GENERAL, layers, count);
#endif

    VkImageMemoryBarrier to_present{};
    to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_present.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = dst;
    to_present.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &to_present);
  } else {
    // Transition swapchain image UNDEFINED/PRESENT_SRC -> TRANSFER_DST_OPTIMAL.
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = dst;
    to_dst.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_dst.subresourceRange.levelCount = 1;
    to_dst.subresourceRange.layerCount = 1;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    // srcStage must be TRANSFER (the stage the acquire semaphore is waited at),
    // not TOP_OF_PIPE: that orders this layout-transition write after the
    // vkAcquireNextImageKHR signal and removes the WRITE_AFTER_READ hazard the
    // sync-validation layer flags (it explicitly suggests including the
    // transfer stage in this barrier's srcStageMask).
    d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_dst);

    for (size_t i = 0; i < count; ++i) {
      const FlutterLayer* layer = layers[i];
      if (!layer) {
        continue;
      }
      if (layer->type == kFlutterLayerContentTypeBackingStore &&
          layer->backing_store) {
        auto* baton =
            static_cast<VulkanStoreBaton*>(layer->backing_store->user_data);
        if (!baton || !baton->store) {
          continue;
        }
        const auto dx = static_cast<int32_t>(layer->offset.x);
        const auto dy = static_cast<int32_t>(layer->offset.y);
        const auto dw = static_cast<int32_t>(layer->size.width);
        const auto dh = static_cast<int32_t>(layer->size.height);
        BlitStoreToSwapchain(cmd, *baton->store, dst, dx, dy, dw, dh);
      } else if (layer->type == kFlutterLayerContentTypePlatformView &&
                 layer->platform_view) {
        ok = PresentPlatformView(layer) && ok;
      }
    }

    // The HUD (if active) transitions TRANSFER_DST->GENERAL and draws; the
    // present transition then sources from GENERAL instead of TRANSFER_DST.
#if BUILD_HUD
    const bool hud_drew =
        MaybeRenderHud(cmd, dst, swapchain_image_views_[image_index],
                       swapchain_extent_.width, swapchain_extent_.height,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, layers, count);
#else
    const bool hud_drew = false;
#endif

    // Transition swapchain image -> PRESENT_SRC_KHR.
    VkImageMemoryBarrier to_present{};
    to_present.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_present.oldLayout = hud_drew ? VK_IMAGE_LAYOUT_GENERAL
                                    : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_present.image = dst;
    to_present.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_present.subresourceRange.levelCount = 1;
    to_present.subresourceRange.layerCount = 1;
    to_present.srcAccessMask = hud_drew ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                        : VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    d().vkCmdPipelineBarrier(cmd,
                             hud_drew
                                 ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                 : VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &to_present);
  }

  d().vkEndCommandBuffer(cmd);

  // Submit waits on image_available (signaled when the swapchain image is
  // presentable), signals render_finished and the slot's in_flight fence.
  // No vkQueueWaitIdle — the next frame's slot wait is the only sync point.
  //
  // Wait stage matches the command buffer's first use of the acquired image:
  // the blend path's first write is the render pass color attachment
  // (COLOR_ATTACHMENT_OUTPUT); the overwrite-blit path's is the
  // UNDEFINED->TRANSFER_DST transition + blit (TRANSFER). Waiting at the wrong
  // stage leaves the acquire unsynchronized with that first write
  // (SYNC-HAZARD-WRITE-AFTER-READ vs vkAcquireNextImageKHR,
  // MissingAcquireWait).
  const VkPipelineStageFlags wait_stage =
      blend ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
            : VK_PIPELINE_STAGE_TRANSFER_BIT;
  // Wait the swapchain-image acquire plus every platform-view producer's
  // acquire fence collected while compositing. The producer's copy is read at
  // the fragment shader (blend) or transfer (blit), so wait its fence before
  // both.
  std::vector<VkSemaphore> wait_sems = {slot.image_available};
  std::vector<VkPipelineStageFlags> wait_stages = {wait_stage};
  for (VkSemaphore s : frame_acquire_waits_) {
    wait_sems.push_back(s);
    wait_stages.push_back(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
  }
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.waitSemaphoreCount = static_cast<uint32_t>(wait_sems.size());
  submit.pWaitSemaphores = wait_sems.data();
  submit.pWaitDstStageMask = wait_stages.data();
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  // Signal (and later present-wait on) the per-image semaphore, not a per-slot
  // one, so it isn't reused while a prior present on this image is pending.
  // Also signal the release semaphore when a platform view was sampled, so its
  // producer/host can tell when this frame is done reading it.
  VkSemaphore& render_finished = m_compositor_render_finished_[image_index];
  std::array<VkSemaphore, 2> signal_sems = {render_finished, release_sem_};
  const bool signal_release =
      !frame_pv_surfaces_.empty() && release_sem_ != VK_NULL_HANDLE;
  submit.signalSemaphoreCount = signal_release ? 2 : 1;
  submit.pSignalSemaphores = signal_sems.data();
  {
    std::lock_guard<std::mutex> queue_lock(queue_mutex_);
    d().vkQueueSubmit(queue_, 1, &submit, slot.in_flight);
  }
  // Export the release fence to each platform view composited this frame.
  PublishReleaseFences();
  // The imported acquire semaphores are now owned by this submit; retire them
  // after a margin so they are destroyed once its fence has signalled.
  for (VkSemaphore s : frame_acquire_waits_) {
    acquire_wait_retire_.emplace_back(acquire_wait_seq_ + 4, s);
  }
  frame_acquire_waits_.clear();

  VkPresentInfoKHR present_info{};
  present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present_info.waitSemaphoreCount = 1;
  present_info.pWaitSemaphores = &render_finished;
  present_info.swapchainCount = 1;
  present_info.pSwapchains = &swapchain_;
  present_info.pImageIndices = &image_index;
  // Mint wp_presentation_feedback before the commit baked into
  // vkQueuePresentKHR. See PresentCallback for rationale.
  RequestPresentationFeedback();
  VkResult result;
  {
    std::lock_guard<std::mutex> queue_lock(queue_mutex_);
    result = d().vkQueuePresentKHR(queue_, &present_info);
  }
  if (result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) {
    resize_pending_ = true;
  }

  ProfilePresent(result == VK_SUCCESS);

  ++m_compositor_current_frame_;
  return ok;
}

bool WaylandVulkanBackend::InitDmabufRing(uint32_t w, uint32_t h) {
  {
    std::lock_guard<std::mutex> queue_lock(queue_mutex_);
    d().vkQueueWaitIdle(queue_);
  }
  for (auto& s : dmabuf_slots_) {
    if (s.fence != VK_NULL_HANDLE) {
      d().vkDestroyFence(device_, s.fence, nullptr);
    }
  }
  dmabuf_slots_.clear();

  if (dmabuf_cmd_pool_ == VK_NULL_HANDLE) {
    VkCommandPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = queue_family_index_;
    if (d().vkCreateCommandPool(device_, &pi, nullptr, &dmabuf_cmd_pool_) !=
        VK_SUCCESS) {
      ihs::log::error("[WaylandVulkanBackend] dma-buf cmd pool create failed");
      dmabuf_present_active_ = false;
      return false;
    }
  }

  constexpr size_t kRing = 3;
  for (size_t i = 0; i < kRing; ++i) {
    DmabufSlot slot;
    std::string err;
    slot.image = wl_vulkan::DmabufImage::Create(
        physical_device_, device_, w, h, VK_FORMAT_B8G8R8A8_UNORM,
        DRM_FORMAT_XRGB8888,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT,
        dmabuf_modifiers_, err);
    if (!slot.image) {
      ihs::log::error("[WaylandVulkanBackend] dma-buf ring image {} failed: {}",
                      i, err);
      dmabuf_present_active_ = false;
      return false;
    }
    slot.buffer = wl_vulkan::WlDmabufBuffer::Create(
        dmabuf_feedback_->CreateParams(), *slot.image);
    if (!slot.buffer) {
      ihs::log::error("[WaylandVulkanBackend] dma-buf ring buffer {} failed",
                      i);
      dmabuf_present_active_ = false;
      return false;
    }
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = dmabuf_cmd_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    d().vkAllocateCommandBuffers(device_, &ai, &slot.cmd);
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // first wait returns immediately
    d().vkCreateFence(device_, &fi, nullptr, &slot.fence);
    slot.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    dmabuf_slots_.push_back(std::move(slot));
  }
  dmabuf_ring_w_ = w;
  dmabuf_ring_h_ = h;
  dmabuf_frame_ = 0;
  ihs::log::info(
      "[WaylandVulkanBackend] dma-buf present ring: {} slots {}x{} modifier "
      "{:#018x}",
      dmabuf_slots_.size(), w, h, dmabuf_slots_.front().image->modifier());
  return true;
}

void WaylandVulkanBackend::WarnMissingPlatformView(
    FlutterPlatformViewIdentifier id) {
  // A platform view may take the wl_subsurface route (handled by the sequencer)
  // or the ICompositorSurface texture route (PresentPlatformView). Warn only
  // when neither is registered for it.
  std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
  if (m_compositor_surfaces.find(id) == m_compositor_surfaces.end()) {
    ihs::log::warn(
        "Vulkan compositor: platform view {} has no registered subsurface or "
        "compositor surface",
        id);
  }
}

bool WaylandVulkanBackend::PresentPlatformView(const FlutterLayer* layer) {
  const auto composed = MutationStack::Compose(layer->platform_view);
  if (composed.NeedsPluginComposite()) {
    ihs::log::debug(
        "Vulkan compositor: platform view {} has non-trivial mutations "
        "(opacity={:.3f} rounded={} perspective={} axis_aligned={}); plugin "
        "OnPresent must apply them.",
        layer->platform_view->identifier, composed.opacity,
        composed.has_rounded_clip, composed.has_perspective,
        composed.IsAxisAligned());
  }
  std::shared_ptr<ICompositorSurface> surface;
  {
    std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
    const auto it =
        m_compositor_surfaces.find(layer->platform_view->identifier);
    if (it != m_compositor_surfaces.end()) {
      surface = it->second;
    }
  }
  // No compositor surface = the subsurface route (or an unregistered PV,
  // already warned via the sequencer). Treat as success so it doesn't fail the
  // present.
  return surface ? surface->OnPresent(layer) : true;
}

void WaylandVulkanBackend::CollectAcquireWait(ICompositorSurface* surface) {
  // Every composited platform view is handed a release fence (so the producer /
  // host can tell when this frame is done reading it). Create the shared,
  // SYNC_FD-exportable release semaphore lazily on first use.
  if (release_sem_ == VK_NULL_HANDLE && !release_sem_failed_) {
    VkExportSemaphoreCreateInfo esi{};
    esi.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO;
    esi.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    VkSemaphoreCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    sci.pNext = &esi;
    if (d().vkCreateSemaphore(device_, &sci, VKALLOC, &release_sem_) !=
        VK_SUCCESS) {
      release_sem_ = VK_NULL_HANDLE;
      release_sem_failed_ = true;
    }
  }
  if (release_sem_ != VK_NULL_HANDLE) {
    frame_pv_surfaces_.push_back(surface);
  }

  const int fd = surface->TakeAcquireFenceFd();
  if (fd < 0) {
    return;  // producer stalled synchronously (or no SYNC_FD support)
  }
  VkSemaphore sem = VK_NULL_HANDLE;
  VkSemaphoreCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  if (d().vkCreateSemaphore(device_, &sci, VKALLOC, &sem) != VK_SUCCESS) {
    close(fd);
    return;
  }
  VkImportSemaphoreFdInfoKHR ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
  ii.semaphore = sem;
  ii.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
  ii.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;  // required for SYNC_FD
  ii.fd = fd;  // consumed by a successful import; owned by us on failure
  if (d().vkImportSemaphoreFdKHR(device_, &ii) != VK_SUCCESS) {
    close(fd);
    d().vkDestroySemaphore(device_, sem, VKALLOC);
    return;
  }
  frame_acquire_waits_.push_back(sem);
}

void WaylandVulkanBackend::ReapAcquireWaits() {
  for (auto it = acquire_wait_retire_.begin();
       it != acquire_wait_retire_.end();) {
    if (acquire_wait_seq_ >= it->first) {
      d().vkDestroySemaphore(device_, it->second, VKALLOC);
      it = acquire_wait_retire_.erase(it);
    } else {
      ++it;
    }
  }
}

void WaylandVulkanBackend::ScheduleDeferredDestroy(std::function<void()> fn) {
  if (!fn) {
    return;
  }
  const std::lock_guard<std::mutex> lock(deferred_destroy_mu_);
  // Free after a margin of presents: by then any frame that bound the resource
  // has been submitted and its fence waited at slot reuse (frames retire in
  // order), and no frame is mid-record (this runs at the top of present).
  deferred_destroys_.emplace_back(deferred_epoch_ + 6, std::move(fn));
}

void WaylandVulkanBackend::ReapDeferredDestroys() {
  std::vector<std::function<void()>> ready;
  {
    const std::lock_guard<std::mutex> lock(deferred_destroy_mu_);
    ++deferred_epoch_;
    for (auto it = deferred_destroys_.begin();
         it != deferred_destroys_.end();) {
      if (deferred_epoch_ >= it->first) {
        ready.push_back(std::move(it->second));
        it = deferred_destroys_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& fn : ready) {
    fn();  // outside the lock: frees an import via the host's importer
  }
}

void WaylandVulkanBackend::PublishReleaseFences() {
  if (frame_pv_surfaces_.empty()) {
    return;
  }
  // release_sem_ was signalled by the frame submit; export a sync_file that
  // fires when the frame retires and give each composited view its own dup.
  int release_fd = -1;
  if (release_sem_ != VK_NULL_HANDLE) {
    VkSemaphoreGetFdInfoKHR gfi{};
    gfi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR;
    gfi.semaphore = release_sem_;
    gfi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    if (d().vkGetSemaphoreFdKHR(device_, &gfi, &release_fd) != VK_SUCCESS) {
      release_fd = -1;
    }
  }
  for (auto* s : frame_pv_surfaces_) {
    s->SetReleaseFenceFd(release_fd >= 0 ? dup(release_fd) : -1);
  }
  if (release_fd >= 0) {
    close(release_fd);
  }
  frame_pv_surfaces_.clear();
}

bool WaylandVulkanBackend::CompositeLayersBlend(VkCommandBuffer cmd,
                                                const FlutterLayer** layers,
                                                size_t count,
                                                VkImageView target_view,
                                                uint32_t width,
                                                uint32_t height,
                                                uint64_t frame) {
  struct Draw {
    VkImage src;
    VkFormat format;
    int32_t dx, dy, dw, dh;
    VulkanBackingStore* store;  // non-null => restore to COLOR_ATTACHMENT after
  };
  std::vector<Draw> draws;
  draws.reserve(count);

  const auto barrier =
      [&](VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
          VkAccessFlags src_access, VkPipelineStageFlags src_stage,
          VkAccessFlags dst_access, VkPipelineStageFlags dst_stage) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = old_layout;
        b.newLayout = new_layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = src_access;
        b.dstAccessMask = dst_access;
        d().vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0,
                                 nullptr, 1, &b);
      };

  bool ok = true;
  // Phase 1: render platform views and move every source to SHADER_READ_ONLY
  // (layout transitions can't happen inside the render pass).
  for (size_t i = 0; i < count; ++i) {
    const FlutterLayer* layer = layers[i];
    if (!layer) {
      continue;
    }
    const auto dx = static_cast<int32_t>(layer->offset.x);
    const auto dy = static_cast<int32_t>(layer->offset.y);
    const auto dw = static_cast<int32_t>(layer->size.width);
    const auto dh = static_cast<int32_t>(layer->size.height);
    if (layer->type == kFlutterLayerContentTypeBackingStore &&
        layer->backing_store) {
      auto* baton =
          static_cast<VulkanStoreBaton*>(layer->backing_store->user_data);
      if (!baton || !baton->store) {
        continue;
      }
      VulkanBackingStore* store = baton->store;
      barrier(store->Image(), store->Layout(),
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      store->SetLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      draws.push_back(
          {store->Image(), VulkanBackingStore::kFormat, dx, dy, dw, dh, store});
    } else if (layer->type == kFlutterLayerContentTypePlatformView &&
               layer->platform_view) {
      ok = PresentPlatformView(layer) && ok;
      std::shared_ptr<ICompositorSurface> surface;
      {
        std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
        const auto it =
            m_compositor_surfaces.find(layer->platform_view->identifier);
        if (it != m_compositor_surfaces.end()) {
          surface = it->second;
        }
      }
      if (!surface) {
        continue;
      }
      int32_t pw = 0;
      int32_t ph = 0;
      void* vk_img = surface->GetVulkanImage(&pw, &ph);
      if (vk_img == nullptr || pw <= 0 || ph <= 0) {
        continue;  // GL / subsurface platform view — nothing to sample
      }
      auto src = reinterpret_cast<VkImage>(vk_img);
      // Wait the producer's acquire fence (if any) in this frame's submit.
      CollectAcquireWait(surface.get());
      const auto cur =
          static_cast<VkImageLayout>(surface->GetVulkanImageLayout());
      if (cur != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        const bool from_preinit = cur == VK_IMAGE_LAYOUT_PREINITIALIZED;
        barrier(src, cur, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                from_preinit ? VK_ACCESS_HOST_WRITE_BIT : 0,
                from_preinit ? VK_PIPELINE_STAGE_HOST_BIT
                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        surface->SetVulkanImageLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
      }
      draws.push_back({src, VK_FORMAT_B8G8R8A8_UNORM, dx, dy, dw, dh, nullptr});
      if (const int64_t us = ivi::pv_latency::FirstCompositeLatencyUs(
              layer->platform_view->identifier);
          us >= 0) {
        ihs::log::info(
            "[pv-latency] view id={} input->first-composite {:.1f} ms",
            layer->platform_view->identifier, static_cast<double>(us) / 1000.0);
      }
    }
  }

  // Phase 2: blend all layers into the target in one render pass (z-order).
  layer_compositor_->BeginFrame(cmd, target_view, width, height, frame);
  for (const Draw& dr : draws) {
    layer_compositor_->DrawLayer(cmd, dr.src, dr.format, dr.dx, dr.dy, dr.dw,
                                 dr.dh);
  }
  wl_vulkan::LayerCompositor::EndFrame(cmd);

  // Phase 3: restore backing stores to COLOR_ATTACHMENT for the engine's next
  // render into them.
  for (const Draw& dr : draws) {
    if (dr.store != nullptr) {
      barrier(dr.src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
      dr.store->SetLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
  }
  // The render pass leaves target_view's image in GENERAL (its finalLayout);
  // the caller transitions from there (present or dma-buf hand-off).
  return ok;
}

void WaylandVulkanBackend::BlitPlatformViewVulkan(VkCommandBuffer cmd,
                                                  const FlutterLayer* layer,
                                                  VkImage dst) {
  std::shared_ptr<ICompositorSurface> surface;
  {
    std::lock_guard<std::mutex> lock(m_compositor_surfaces_mu_);
    const auto it =
        m_compositor_surfaces.find(layer->platform_view->identifier);
    if (it != m_compositor_surfaces.end()) {
      surface = it->second;
    }
  }
  if (!surface) {
    return;
  }
  int32_t pw = 0;
  int32_t ph = 0;
  void* vk_img = surface->GetVulkanImage(&pw, &ph);
  if (vk_img == nullptr || pw <= 0 || ph <= 0) {
    return;  // GL / subsurface platform view — nothing to blit here.
  }
  auto src = reinterpret_cast<VkImage>(vk_img);
  // Wait the producer's acquire fence (if any) in this frame's submit.
  CollectAcquireWait(surface.get());
  const bool external = surface->NeedsExternalQueueAcquire();

  if (external) {
    // An imported dma-buf the producer rewrites every frame through an aliased
    // image. Acquire ownership from VK_QUEUE_FAMILY_EXTERNAL and move it to a
    // transfer source each frame (the producer released it to EXTERNAL after
    // its render); it is handed back after the blit below.
    VkImageMemoryBarrier acq{};
    acq.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    acq.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    acq.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    acq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    acq.dstQueueFamilyIndex = queue_family_index_;
    acq.srcAccessMask = 0;  // the release on the producer side owns src access
    acq.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    acq.image = src;
    acq.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &acq);
  } else {
    // The plugin's static image starts PREINITIALIZED (host-written, contents
    // preserved). Transition it to a transfer source ONCE — a static image left
    // in TRANSFER_SRC must not be re-transitioned every frame (that both
    // corrupts the read and races the prior frame's blit). The surface tracks
    // the layout.
    const auto cur =
        static_cast<VkImageLayout>(surface->GetVulkanImageLayout());
    if (cur != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
      const bool from_preinit = cur == VK_IMAGE_LAYOUT_PREINITIALIZED;
      VkImageMemoryBarrier to_src{};
      to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      to_src.oldLayout = cur;
      to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_src.image = src;
      to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      // PREINITIALIZED carries host writes; make them visible to the transfer.
      to_src.srcAccessMask = from_preinit ? VK_ACCESS_HOST_WRITE_BIT
                                          : VK_ACCESS_TRANSFER_WRITE_BIT;
      to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      d().vkCmdPipelineBarrier(cmd,
                               from_preinit ? VK_PIPELINE_STAGE_HOST_BIT
                                            : VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                               nullptr, 1, &to_src);
      surface->SetVulkanImageLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
  }

  // Blit the full platform-view image into the slot at the layer rect (scaling
  // to size). dst is already in TRANSFER_DST_OPTIMAL for the backing-store
  // blits in the same command buffer.
  const auto dx = static_cast<int32_t>(layer->offset.x);
  const auto dy = static_cast<int32_t>(layer->offset.y);
  const auto dw = static_cast<int32_t>(layer->size.width);
  const auto dh = static_cast<int32_t>(layer->size.height);
  VkImageBlit region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.srcOffsets[0] = {0, 0, 0};
  region.srcOffsets[1] = {pw, ph, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstOffsets[0] = {dx, dy, 0};
  region.dstOffsets[1] = {dx + dw, dy + dh, 1};
  d().vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                     VK_FILTER_LINEAR);

  if (external) {
    // Release ownership back to VK_QUEUE_FAMILY_EXTERNAL so the producer can
    // rewrite the buffer for the next frame.
    VkImageMemoryBarrier rel{};
    rel.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    rel.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    rel.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    rel.srcQueueFamilyIndex = queue_family_index_;
    rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    rel.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    rel.dstAccessMask = 0;  // the acquire on the producer side owns dst access
    rel.image = src;
    rel.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &rel);
  }
}

void WaylandVulkanBackend::OnFrameDone(void* data,
                                       wl_callback* cb,
                                       uint32_t /*time*/) {
  auto* self = static_cast<WaylandVulkanBackend*>(data);
  {
    std::lock_guard<std::mutex> lock(self->frame_mu_);
    // Only the current in-flight callback advances pacing; a stale one (a prior
    // frame whose callback fired after this frame's present timed out waiting)
    // is still retired, just without touching state.
    if (self->frame_callback_ == cb) {
      self->frame_callback_ = nullptr;
      self->frame_ready_ = true;
    }
  }
  wl_callback_destroy(cb);
  self->frame_cv_.notify_one();
}

bool WaylandVulkanBackend::PresentLayersDmabuf(const FlutterLayer** layers,
                                               size_t count) {
  const uint32_t w = width_;
  const uint32_t h = height_;
  if (dmabuf_slots_.empty() || dmabuf_ring_w_ != w || dmabuf_ring_h_ != h) {
    if (!InitDmabufRing(w, h)) {
      // Ring allocation failed. There is no swapchain to fall back to at
      // runtime — it was skipped in CreateSurface when the dma-buf path was
      // selected — so drop this frame. Rare: dma-buf allocation only fails
      // under GPU memory pressure, and InitDmabufRing has already cleared
      // dmabuf_present_active_.
      ihs::log::error(
          "[WaylandVulkanBackend] dma-buf ring init failed; frame dropped");
      return false;
    }
  }
  // Refresh pacing. Block until the previous frame's wl_surface.frame callback
  // fired — the compositor signalling it is ready for the next frame — before
  // producing this one. This is the dma-buf path's backpressure; the swapchain
  // path gets the equivalent from vkQueuePresentKHR blocking at vblank under
  // FIFO. Bounded so an occluded/unmapped surface (whose callbacks stop firing)
  // degrades to ~10 fps free-running instead of hanging the rasterizer.
  {
    std::unique_lock<std::mutex> lock(frame_mu_);
    frame_cv_.wait_for(lock, std::chrono::milliseconds(100),
                       [this] { return frame_ready_; });
  }
  // Prefer a slot whose wl_buffer the compositor has released, so we don't
  // overwrite a buffer that is still being scanned out. Start at the
  // round-robin position and take the first free slot; with refresh pacing the
  // previous buffer is normally released by now, but fall back to the
  // round-robin slot if none is free.
  size_t idx = dmabuf_frame_ % dmabuf_slots_.size();
  for (size_t i = 0; i < dmabuf_slots_.size(); ++i) {
    const size_t cand = (dmabuf_frame_ + i) % dmabuf_slots_.size();
    // On the explicit-sync path the compositor signals a release timeline
    // instead of sending wl_buffer.release, so poll that; otherwise use the
    // wl_buffer.release flag.
    const bool released =
        explicit_sync_
            ? explicit_sync_->SlotReleased(dmabuf_slots_[cand].release_point)
            : dmabuf_slots_[cand].buffer->released();
    if (released) {
      idx = cand;
      break;
    }
  }
  DmabufSlot& slot = dmabuf_slots_[idx];

  CHECK_VK_RESULT(
      d().vkWaitForFences(device_, 1, &slot.fence, VK_TRUE, UINT64_MAX));
  CHECK_VK_RESULT(d().vkResetFences(device_, 1, &slot.fence));

  VkCommandBuffer cmd = slot.cmd;
  d().vkResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  d().vkBeginCommandBuffer(cmd, &begin);

  // Explicit-sync acquire bookkeeping (see PresentLayersImpl): retire completed
  // semaphores and start this frame's wait list.
  ReapAcquireWaits();
  ReapDeferredDestroys();
  ++acquire_wait_seq_;
  frame_acquire_waits_.clear();
  frame_pv_surfaces_.clear();

  // Sequence platform-view subsurface Z-order for this frame (both paths): the
  // backing-store layers compose into the dma-buf image, platform views
  // composite via their own subsurfaces (sequencer) or their VkImage (below).
  m_sequencer.Present(layers, count, nullptr,
                      [this](FlutterPlatformViewIdentifier id) {
                        WarnMissingPlatformView(id);
                      });

  bool ok = true;
  if (layer_compositor_) {
    // Correct src-over alpha compositing (a platform view under a transparent
    // Flutter overlay blends instead of being overwritten to black).
    ok = CompositeLayersBlend(cmd, layers, count, slot.image->view(), w, h,
                              dmabuf_frame_);
    slot.layout = VK_IMAGE_LAYOUT_GENERAL;  // render pass finalLayout
  } else {
    // Fallback: overwrite blits. Correct only when layers don't overlap; a
    // transparent overlay over a platform view erases it. Transition the slot
    // to TRANSFER_DST for the blits, then to GENERAL for the present.
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = slot.layout;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = slot.image->image();
    to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &to_dst);

    // Overlapping slot writes must run in layer order; barrier between them.
    bool slot_written = false;
    const auto order_slot_write = [&] {
      if (!slot_written) {
        slot_written = true;
        return;
      }
      VkImageMemoryBarrier waw{};
      waw.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      waw.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      waw.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      waw.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      waw.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      waw.image = slot.image->image();
      waw.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      waw.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      waw.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                               nullptr, 1, &waw);
    };

    for (size_t i = 0; i < count; ++i) {
      const FlutterLayer* layer = layers[i];
      if (!layer) {
        continue;
      }
      if (layer->type == kFlutterLayerContentTypeBackingStore &&
          layer->backing_store) {
        auto* baton =
            static_cast<VulkanStoreBaton*>(layer->backing_store->user_data);
        if (!baton || !baton->store) {
          continue;
        }
        const auto dx = static_cast<int32_t>(layer->offset.x);
        const auto dy = static_cast<int32_t>(layer->offset.y);
        const auto dw = static_cast<int32_t>(layer->size.width);
        const auto dh = static_cast<int32_t>(layer->size.height);
        order_slot_write();
        BlitStoreToSwapchain(cmd, *baton->store, slot.image->image(), dx, dy,
                             dw, dh);
      } else if (layer->type == kFlutterLayerContentTypePlatformView &&
                 layer->platform_view) {
        ok = PresentPlatformView(layer) && ok;
        order_slot_write();
        BlitPlatformViewVulkan(cmd, layer, slot.image->image());
      }
    }

    VkImageMemoryBarrier to_general{};
    to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_general.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_general.image = slot.image->image();
    to_general.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    to_general.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_general.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &to_general);
    slot.layout = VK_IMAGE_LAYOUT_GENERAL;
  }

#if BUILD_HUD
  // Draw the debug HUD over the composited slot (already GENERAL, which is also
  // the layout the exported dma-buf is scanned out in, so no post-transition).
  MaybeRenderHud(cmd, slot.image->image(), slot.image->view(), w, h,
                 VK_IMAGE_LAYOUT_GENERAL, layers, count);
#endif

  d().vkEndCommandBuffer(cmd);

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;

  // Wait every platform-view producer's acquire fence collected while
  // compositing (binary sync_file semaphores), before the fragment/transfer
  // read of the view.
  const std::vector<VkPipelineStageFlags> pv_wait_stages(
      frame_acquire_waits_.size(),
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
  if (!frame_acquire_waits_.empty()) {
    submit.waitSemaphoreCount =
        static_cast<uint32_t>(frame_acquire_waits_.size());
    submit.pWaitSemaphores = frame_acquire_waits_.data();
    submit.pWaitDstStageMask = pv_wait_stages.data();
  }

  // Explicit sync: the blit submit signals the acquire timeline at this frame's
  // point (the compositor waits on it before sampling), so the CPU stall below
  // is skipped. The fence still fires for command-buffer/image reuse safety at
  // slot selection. Without explicit sync, the CPU fence is the producer sync.
  wl_vulkan::ExplicitSync::FramePoints sync_pts{};
  VkTimelineSemaphoreSubmitInfo tl{};
  // Signal the wl compositor's acquire timeline (explicit sync) and/or the
  // platform-view release semaphore (binary; value ignored). Build one array so
  // both can ride the same submit.
  std::array<VkSemaphore, 2> signal_sems{};
  std::array<uint64_t, 2> signal_vals{};
  uint32_t signal_count = 0;
  if (explicit_sync_) {
    sync_pts = explicit_sync_->NextFrame();
    signal_sems[signal_count] = explicit_sync_->acquire_semaphore();
    signal_vals[signal_count] = sync_pts.acquire;
    ++signal_count;
  }
  const bool signal_release =
      !frame_pv_surfaces_.empty() && release_sem_ != VK_NULL_HANDLE;
  if (signal_release) {
    signal_sems[signal_count] = release_sem_;
    signal_vals[signal_count] = 0;  // ignored for a binary semaphore
    ++signal_count;
  }
  if (signal_count > 0) {
    submit.signalSemaphoreCount = signal_count;
    submit.pSignalSemaphores = signal_sems.data();
    if (explicit_sync_) {  // a timeline signal needs the values sidecar
      tl.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
      tl.signalSemaphoreValueCount = signal_count;
      tl.pSignalSemaphoreValues = signal_vals.data();
      submit.pNext = &tl;
    }
  }
  {
    std::lock_guard<std::mutex> queue_lock(queue_mutex_);
    d().vkQueueSubmit(queue_, 1, &submit, slot.fence);
  }
  PublishReleaseFences();
  if (!explicit_sync_) {
    // CPU fence: block until the blit completes so the dma-buf holds a full
    // frame before the compositor samples it. Explicit sync replaces this stall
    // with the acquire timeline point the compositor waits on GPU-side.
    CHECK_VK_RESULT(
        d().vkWaitForFences(device_, 1, &slot.fence, VK_TRUE, UINT64_MAX));
  }
  // The imported acquire semaphores are owned by this submit now; retire them
  // to be destroyed once its fence has signalled.
  for (VkSemaphore s : frame_acquire_waits_) {
    acquire_wait_retire_.emplace_back(acquire_wait_seq_ + 4, s);
  }
  frame_acquire_waits_.clear();

  RequestPresentationFeedback();

  wl_surface* surface = wl_surface_.load(std::memory_order_acquire);
  // Arm the pacing callback for this commit, so the next frame's present waits
  // on it. Request it before the commit it is paired with. A prior callback
  // still in flight (this frame's wait timed out) is left to fire and self-
  // retire in OnFrameDone; only the newest one advances pacing.
  {
    static const wl_callback_listener kFrameListener = {
        &WaylandVulkanBackend::OnFrameDone};
    std::lock_guard<std::mutex> lock(frame_mu_);
    frame_callback_ = wl_surface_frame(surface);
    wl_callback_add_listener(frame_callback_, &kFrameListener, this);
    frame_ready_ = false;
  }
  auto* buffer = reinterpret_cast<wl_buffer*>(slot.buffer->proxy());
  wl_surface_attach(surface, buffer, 0, 0);
  // Damage the surface. wl_surface.damage_buffer (buffer coordinates) is a v4
  // request; ivi binds wl_compositor at v3 today, so calling it there is a
  // FATAL protocol error that disconnects the client. Fall back to
  // wl_surface.damage (v1, surface coordinates) on older surfaces. This branch
  // is version-driven, so it auto-upgrades to the precise buffer-coordinate
  // path (needed for partial-repaint damage) if the compositor bind is ever
  // raised to v4+. Full-surface damage is equivalent under either request.
  if (wl_proxy_get_version(reinterpret_cast<wl_proxy*>(surface)) >= 4) {
    wl_surface_damage_buffer(surface, 0, 0, static_cast<int32_t>(w),
                             static_cast<int32_t>(h));
  } else {
    wl_surface_damage(surface, 0, 0, INT32_MAX, INT32_MAX);
  }
  // Explicit sync: attach the acquire/release points to this commit (must be
  // between attach and commit) and record the slot's release point for the
  // reuse poll. The compositor waits on acquire before sampling and signals
  // release when done.
  if (explicit_sync_) {
    explicit_sync_->SetSurfacePoints(sync_pts);
    slot.release_point = sync_pts.release;
  }
  wl_surface_commit(surface);
  wl_display_flush(wl_display_);
  slot.buffer->mark_in_use();

  ProfilePresent(true);
  ++dmabuf_frame_;
  return ok;
}

void WaylandVulkanBackend::CompositorPipeliningInit() {
  CompositorPipeliningCleanup();

  if (swapchain_images_.empty()) {
    return;
  }

  VkCommandPoolCreateInfo pool_info{};
  pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_info.queueFamilyIndex = queue_family_index_;
  if (d().vkCreateCommandPool(device_, &pool_info, nullptr,
                              &m_compositor_cmd_pool_) != VK_SUCCESS) {
    ihs::log::error("Vulkan compositor: failed to create cmd pool");
    return;
  }

  const auto slot_count = swapchain_images_.size();
  m_compositor_slots_.resize(slot_count);

  VkCommandBufferAllocateInfo cb_alloc{};
  cb_alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cb_alloc.commandPool = m_compositor_cmd_pool_;
  cb_alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_alloc.commandBufferCount = static_cast<uint32_t>(slot_count);
  std::vector<VkCommandBuffer> cmds(slot_count);
  CHECK_VK_RESULT(
      d().vkAllocateCommandBuffers(device_, &cb_alloc, cmds.data()));

  // Fences start signaled so the first wait in PresentLayersImpl returns
  // immediately for every slot.
  VkFenceCreateInfo fence_info{};
  fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  VkSemaphoreCreateInfo sem_info{};
  sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

  for (size_t i = 0; i < slot_count; ++i) {
    auto& s = m_compositor_slots_[i];
    s.cmd_buffer = cmds[i];
    CHECK_VK_RESULT(
        d().vkCreateFence(device_, &fence_info, nullptr, &s.in_flight));
    CHECK_VK_RESULT(
        d().vkCreateSemaphore(device_, &sem_info, nullptr, &s.image_available));
  }

  // One present-wait semaphore per swapchain image (see header). Count tracks
  // the swapchain, which here equals slot_count.
  m_compositor_render_finished_.resize(slot_count, VK_NULL_HANDLE);
  for (auto& sem : m_compositor_render_finished_) {
    CHECK_VK_RESULT(d().vkCreateSemaphore(device_, &sem_info, nullptr, &sem));
  }
  m_compositor_current_frame_ = 0;
}

void WaylandVulkanBackend::CompositorPipeliningCleanup() {
  if (device_ == VK_NULL_HANDLE) {
    return;
  }
  // Make sure no slot's resources are still in flight before tearing them
  // down. Cheap: only fires at swapchain recreation or backend shutdown.
  if (!m_compositor_slots_.empty()) {
    d().vkDeviceWaitIdle(device_);
  }
  for (auto& s : m_compositor_slots_) {
    if (s.image_available) {
      d().vkDestroySemaphore(device_, s.image_available, nullptr);
    }
    if (s.in_flight) {
      d().vkDestroyFence(device_, s.in_flight, nullptr);
    }
    s = {};
  }
  m_compositor_slots_.clear();
  for (auto sem : m_compositor_render_finished_) {
    if (sem != VK_NULL_HANDLE) {
      d().vkDestroySemaphore(device_, sem, nullptr);
    }
  }
  m_compositor_render_finished_.clear();
  if (m_compositor_cmd_pool_ != VK_NULL_HANDLE) {
    d().vkDestroyCommandPool(device_, m_compositor_cmd_pool_, nullptr);
    m_compositor_cmd_pool_ = VK_NULL_HANDLE;
  }
  m_compositor_current_frame_ = 0;
}

#endif  // BUILD_COMPOSITOR
