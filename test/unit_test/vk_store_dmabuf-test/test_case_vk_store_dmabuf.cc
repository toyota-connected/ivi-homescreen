/*
 * Copyright 2026 Toyota Connected North America
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

#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include "gtest/gtest.h"

#include "backend/wayland_vulkan/vulkan_backing_store.h"

namespace {

const auto& d() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

std::vector<std::string>* g_messages = nullptr;

VKAPI_ATTR VkBool32 VKAPI_CALL
Record(VkDebugUtilsMessageSeverityFlagBitsEXT,
       VkDebugUtilsMessageTypeFlagsEXT,
       const VkDebugUtilsMessengerCallbackDataEXT* data,
       void*) {
  if (g_messages != nullptr) {
    g_messages->emplace_back(
        data->pMessageIdName != nullptr ? data->pMessageIdName : "(unnamed)");
  }
  return VK_FALSE;
}

bool HasExtension(const std::vector<VkExtensionProperties>& have,
                  const char* want) {
  for (const auto& e : have) {
    if (strcmp(e.extensionName, want) == 0) {
      return true;
    }
  }
  return false;
}

// A dma-buf export is only in spec when the driver reports the handle type
// compatible with the exact image the store then creates. This is the same
// query VUID-VkImageCreateInfo-pNext-00990 and
// VUID-VkExportMemoryAllocateInfo-handleTypes-09860 are checked against.
bool QueryExportable(VkPhysicalDevice gpu,
                     VkImageTiling tiling,
                     std::optional<uint64_t> modifier) {
  VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod{};
  mod.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
  mod.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkPhysicalDeviceExternalImageFormatInfo ext{};
  ext.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
  ext.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  if (modifier) {
    mod.drmFormatModifier = *modifier;
    ext.pNext = &mod;
  }
  VkPhysicalDeviceImageFormatInfo2 info{};
  info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
  info.pNext = &ext;
  info.format = VulkanBackingStore::kFormat;
  info.type = VK_IMAGE_TYPE_2D;
  info.tiling = tiling;
  info.usage = VulkanBackingStore::kUsage;
  VkExternalImageFormatProperties efp{};
  efp.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
  VkImageFormatProperties2 props{};
  props.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
  props.pNext = &efp;
  if (d().vkGetPhysicalDeviceImageFormatProperties2(gpu, &info, &props) !=
      VK_SUCCESS) {
    return false;
  }
  const auto& p = efp.externalMemoryProperties;
  return (p.externalMemoryFeatures &
          VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0 &&
         (p.compatibleHandleTypes &
          VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) != 0;
}

// VulkanBackingStore's dma-buf export on a real device. Skips itself without a
// Vulkan loader, without a device, or on a device that cannot export at all;
// the validation-layer assertion additionally skips when the layer is not
// installed.
class VkStoreDmaBuf : public ::testing::Test {
 protected:
  void SetUp() override {
    try {
      VULKAN_HPP_DEFAULT_DISPATCHER.init();
    } catch (const std::exception& e) {
      GTEST_SKIP() << "no Vulkan loader: " << e.what();
    }

    uint32_t layer_count = 0;
    d().vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    d().vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
    for (const auto& l : layers) {
      if (strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
        have_validation_ = true;
      }
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    const char* layer_names[] = {"VK_LAYER_KHRONOS_validation"};
    const char* ext_names[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (have_validation_) {
      ici.enabledLayerCount = 1;
      ici.ppEnabledLayerNames = layer_names;
      ici.enabledExtensionCount = 1;
      ici.ppEnabledExtensionNames = ext_names;
    }
    if (d().vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
      instance_ = VK_NULL_HANDLE;
      GTEST_SKIP() << "vkCreateInstance failed";
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance_));

    if (have_validation_) {
      g_messages = &messages_;
      VkDebugUtilsMessengerCreateInfoEXT mci{};
      mci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
      mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
      mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
      mci.pfnUserCallback = Record;
      d().vkCreateDebugUtilsMessengerEXT(instance_, &mci, nullptr, &messenger_);
    }

    uint32_t n = 0;
    d().vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    d().vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    std::vector<const char*> device_extensions;
    for (VkPhysicalDevice pd : pds) {
      uint32_t qn = 0;
      d().vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> qs(qn);
      d().vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
      bool graphics = false;
      for (uint32_t i = 0; i < qn; ++i) {
        if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
          graphics = true;
          family_ = i;
          break;
        }
      }
      if (!graphics) {
        continue;
      }
      uint32_t en = 0;
      d().vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
      std::vector<VkExtensionProperties> exts(en);
      d().vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, exts.data());
      if (!HasExtension(exts, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) ||
          !HasExtension(exts, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME)) {
        continue;
      }
      device_extensions = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                           VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME};
      have_modifier_ext_ =
          HasExtension(exts, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
      if (have_modifier_ext_) {
        device_extensions.push_back(
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
      }
      physical_ = pd;
      break;
    }
    if (physical_ == VK_NULL_HANDLE) {
      GTEST_SKIP() << "no Vulkan device that can export a dma-buf";
    }

    const float prio = 1.0F;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    dci.ppEnabledExtensionNames = device_extensions.data();
    ASSERT_EQ(d().vkCreateDevice(physical_, &dci, nullptr, &device_),
              VK_SUCCESS);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device_));

    plan_ = VulkanBackingStore::PlanExport(physical_, have_modifier_ext_);
  }

  void TearDown() override {
    if (device_ != VK_NULL_HANDLE) {
      d().vkDestroyDevice(device_, nullptr);
    }
    if (messenger_ != VK_NULL_HANDLE) {
      d().vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    }
    g_messages = nullptr;
    if (instance_ != VK_NULL_HANDLE) {
      d().vkDestroyInstance(instance_, nullptr);
    }
  }

  VkInstance instance_{VK_NULL_HANDLE};
  VkDebugUtilsMessengerEXT messenger_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_{VK_NULL_HANDLE};
  VkDevice device_{VK_NULL_HANDLE};
  uint32_t family_{0};
  bool have_validation_{false};
  bool have_modifier_ext_{false};
  VulkanStoreExportPlan plan_{};
  std::vector<std::string> messages_{};
};

// The plan is only enabled for a configuration the driver reports
// dma-buf-exportable. The bug this replaces assumed TILING_OPTIMAL, which Mesa
// answers with VK_ERROR_FORMAT_NOT_SUPPORTED.
TEST_F(VkStoreDmaBuf, PlansOnlyAnExportableConfiguration) {
  if (!plan_.enabled) {
    GTEST_SKIP() << "no exportable configuration on this driver";
  }
  if (plan_.tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
    ASSERT_FALSE(plan_.modifiers.empty());
    for (const uint64_t m : plan_.modifiers) {
      EXPECT_TRUE(QueryExportable(physical_, plan_.tiling, m))
          << "planned modifier " << m << " is not dma-buf-exportable";
    }
  } else {
    EXPECT_TRUE(QueryExportable(physical_, plan_.tiling, std::nullopt));
    EXPECT_TRUE(plan_.modifiers.empty());
  }
}

// An fd alone does not describe a buffer: on the modifier path the store has to
// report which modifier the driver chose and where the planes are.
TEST_F(VkStoreDmaBuf, ExportedStoreDescribesItsLayout) {
  VulkanBackingStore store(64, 32, device_, physical_, plan_);
  ASSERT_TRUE(store.IsValid());
  if (!store.has_dma_buf()) {
    GTEST_SKIP() << "driver declined the export";
  }
  if (plan_.tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
    GTEST_SKIP() << "opaque export path has no layout to report";
  }
  ASSERT_TRUE(store.dma_buf_modifier().has_value());
  ASSERT_FALSE(store.dma_buf_planes().empty());
  EXPECT_GE(store.dma_buf_planes()[0].stride, 64U * 4U);
}

// A disabled plan is not an error: the store still renders, it just has no fd.
TEST_F(VkStoreDmaBuf, StoreWithoutAnExportIsStillValid) {
  const VulkanStoreExportPlan off{};
  VulkanBackingStore store(64, 32, device_, physical_, off);
  ASSERT_TRUE(store.IsValid());
  EXPECT_FALSE(store.has_dma_buf());
  EXPECT_FALSE(store.dma_buf_modifier().has_value());
  EXPECT_TRUE(store.dma_buf_planes().empty());
}

// The regression this fixes, measured the way it was reported: creating an
// exporting store emits no validation message at all.
TEST_F(VkStoreDmaBuf, CreatingAnExportingStoreIsValidationClean) {
  if (!have_validation_) {
    GTEST_SKIP() << "VK_LAYER_KHRONOS_validation not installed";
  }
  if (!plan_.enabled) {
    GTEST_SKIP() << "no exportable configuration on this driver";
  }
  messages_.clear();
  {
    VulkanBackingStore store(64, 32, device_, physical_, plan_);
    ASSERT_TRUE(store.IsValid());
  }
  EXPECT_TRUE(messages_.empty())
      << "first of " << messages_.size() << ": " << messages_.front();
}

}  // namespace
