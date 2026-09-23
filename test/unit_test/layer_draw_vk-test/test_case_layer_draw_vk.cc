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

#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

#include "gtest/gtest.h"

#include "backend/wayland_vulkan/wl_layer_compositor.h"
#include "view/layer_geometry.h"

namespace {

// The dispatcher LayerCompositor resolves through; this test initializes it.
const auto& d() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

constexpr int kW = 5;  // the image a layer shows, before any transform
constexpr int kH = 3;
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

// Offset of pixel (x, y) in a row-major array of `n`-component pixels, `w`
// pixels wide. Every operand widened, so no index is formed in int.
size_t Px(int x, int y, int w, int n = 4) {
  return (static_cast<size_t>(y) * static_cast<size_t>(w) +
          static_cast<size_t>(x)) *
         static_cast<size_t>(n);
}

std::array<uint8_t, 4> ColorOf(int i) {
  return {static_cast<uint8_t>(20 + i * 10), static_cast<uint8_t>(200 - i * 7),
          static_cast<uint8_t>(i * 13 % 256), 255};
}

// Where a producer puts image pixel (x, y) in its buffer for `t`, from the
// transform's definition: rotate counter-clockwise by 90-degree steps,
// mirroring first for FLIPPED.
std::pair<int, int> ProducerPlaces(BufferTransform t, int x, int y) {
  if (static_cast<uint32_t>(t) >= 4) {
    x = kW - 1 - x;
  }
  switch (static_cast<uint32_t>(t) % 4) {
    case 0:
      return {x, y};
    case 1:
      return {y, kW - 1 - x};
    case 2:
      return {kW - 1 - x, kH - 1 - y};
    default:
      return {kH - 1 - y, x};
  }
}

class LayerDrawVk : public ::testing::Test {
 protected:
  void SetUp() override {
    try {
      VULKAN_HPP_DEFAULT_DISPATCHER.init();
    } catch (const std::exception& e) {
      GTEST_SKIP() << "no Vulkan loader: " << e.what();
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (d().vkCreateInstance(&ici, nullptr, &instance_) != VK_SUCCESS) {
      instance_ = VK_NULL_HANDLE;
      GTEST_SKIP() << "vkCreateInstance failed";
    }
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(instance_));

    uint32_t n = 0;
    d().vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    d().vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    for (VkPhysicalDevice pd : pds) {
      uint32_t qn = 0;
      d().vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
      std::vector<VkQueueFamilyProperties> qs(qn);
      d().vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
      for (uint32_t i = 0; i < qn; ++i) {
        if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0U) {
          physical_ = pd;
          family_ = i;
          break;
        }
      }
      if (physical_ != VK_NULL_HANDLE) {
        break;
      }
    }
    if (physical_ == VK_NULL_HANDLE) {
      GTEST_SKIP() << "no Vulkan device with a graphics queue";
    }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    ASSERT_EQ(d().vkCreateDevice(physical_, &dci, nullptr, &device_),
              VK_SUCCESS);
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(device_));
    d().vkGetDeviceQueue(device_, family_, 0, &queue_);
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family_;
    ASSERT_EQ(d().vkCreateCommandPool(device_, &pci, nullptr, &pool_),
              VK_SUCCESS);
  }

  void TearDown() override {
    if (device_ != VK_NULL_HANDLE) {
      d().vkDeviceWaitIdle(device_);
      for (auto& [img, mem] : images_) {
        d().vkDestroyImage(device_, img, nullptr);
        d().vkFreeMemory(device_, mem, nullptr);
      }
      for (VkImageView v : views_) {
        d().vkDestroyImageView(device_, v, nullptr);
      }
      for (auto& [buf, mem] : buffers_) {
        d().vkDestroyBuffer(device_, buf, nullptr);
        d().vkFreeMemory(device_, mem, nullptr);
      }
      compositors_.clear();  // before the device it was built on
      if (pool_ != VK_NULL_HANDLE) {
        d().vkDestroyCommandPool(device_, pool_, nullptr);
      }
      d().vkDestroyDevice(device_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
      d().vkDestroyInstance(instance_, nullptr);
    }
  }

  [[nodiscard]] uint32_t MemoryType(uint32_t bits,
                                    VkMemoryPropertyFlags want) const {
    VkPhysicalDeviceMemoryProperties mp{};
    d().vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
      if ((bits & (1U << i)) != 0U &&
          (mp.memoryTypes[i].propertyFlags & want) == want) {
        return i;
      }
    }
    return UINT32_MAX;
  }

  // A host-visible buffer holding `bytes` (or zeros).
  VkBuffer HostBuffer(size_t size, const void* bytes, VkDeviceMemory* mem_out) {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    EXPECT_EQ(d().vkCreateBuffer(device_, &bci, nullptr, &buf), VK_SUCCESS);
    VkMemoryRequirements req{};
    d().vkGetBufferMemoryRequirements(device_, buf, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = MemoryType(req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    EXPECT_EQ(d().vkAllocateMemory(device_, &mai, nullptr, &mem), VK_SUCCESS);
    d().vkBindBufferMemory(device_, buf, mem, 0);
    void* p = nullptr;
    d().vkMapMemory(device_, mem, 0, size, 0, &p);
    if (bytes != nullptr) {
      std::memcpy(p, bytes, size);
    } else {
      std::memset(p, 0, size);
    }
    d().vkUnmapMemory(device_, mem);
    buffers_.emplace_back(buf, mem);
    *mem_out = mem;
    return buf;
  }

  VkImage Image(int w, int h, VkImageUsageFlags usage) {
    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = kFormat;
    ci.extent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    EXPECT_EQ(d().vkCreateImage(device_, &ci, nullptr, &img), VK_SUCCESS);
    VkMemoryRequirements req{};
    d().vkGetImageMemoryRequirements(device_, img, &req);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex =
        MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    EXPECT_EQ(d().vkAllocateMemory(device_, &mai, nullptr, &mem), VK_SUCCESS);
    d().vkBindImageMemory(device_, img, mem, 0);
    images_.emplace_back(img, mem);
    return img;
  }

  static void Barrier(VkCommandBuffer cmd,
                      VkImage img,
                      VkImageLayout from,
                      VkImageLayout to) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    d().vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                             0, nullptr, 1, &b);
  }

  static VkBufferImageCopy Region(int w, int h) {
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1};
    return r;
  }

  // Draw a bw x bh buffer (rows first-row-first, as a dma-buf import is) into
  // a kW x kH target through `uv`, and read the target back top row first.
  // `backdrop` prefills the target (kPreserve) so blending is visible.
  std::vector<uint8_t> Draw(int bw,
                            int bh,
                            const std::vector<uint8_t>& rgba,
                            const UvAffine& uv,
                            bool opaque,
                            const std::array<float, 4>* backdrop = nullptr) {
    std::string err;
    auto comp = wl_vulkan::LayerCompositor::Create(
        device_, kFormat, err,
        backdrop != nullptr ? wl_vulkan::LayerCompositor::ContentMode::kPreserve
                            : wl_vulkan::LayerCompositor::ContentMode::kClear);
    EXPECT_NE(comp, nullptr) << err;
    if (comp == nullptr) {
      return {};
    }
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VkBuffer staging = HostBuffer(rgba.size(), rgba.data(), &staging_mem);
    VkDeviceMemory readback_mem = VK_NULL_HANDLE;
    const auto out_size = static_cast<size_t>(kW) * kH * 4;
    VkBuffer readback = HostBuffer(out_size, nullptr, &readback_mem);
    VkImage src = Image(
        bw, bh, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    VkImage dst = Image(kW, kH,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = dst;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = kFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView dst_view = VK_NULL_HANDLE;
    EXPECT_EQ(d().vkCreateImageView(device_, &vci, nullptr, &dst_view),
              VK_SUCCESS);
    views_.push_back(dst_view);

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    d().vkAllocateCommandBuffers(device_, &cai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    d().vkBeginCommandBuffer(cmd, &bi);

    Barrier(cmd, src, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkBufferImageCopy up = Region(bw, bh);
    d().vkCmdCopyBufferToImage(cmd, staging, src,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
    Barrier(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (backdrop != nullptr) {
      Barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      VkClearColorValue cv{};
      std::memcpy(cv.float32, backdrop->data(), sizeof(cv.float32));
      const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                          1};
      d().vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               &cv, 1, &range);
      Barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    } else {
      Barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    EXPECT_TRUE(comp->BeginFrame(cmd, dst_view, kW, kH, /*frame=*/0));
    comp->DrawLayer(cmd, src, kFormat,
                    VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY,
                    VK_SAMPLER_YCBCR_RANGE_ITU_FULL, 0, 0, kW, kH, uv, opaque);
    wl_vulkan::LayerCompositor::EndFrame(cmd);
    // The render pass leaves the target in GENERAL.
    Barrier(cmd, dst, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy down = Region(kW, kH);
    d().vkCmdCopyImageToBuffer(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback, 1, &down);
    d().vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    EXPECT_EQ(d().vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE), VK_SUCCESS);
    d().vkQueueWaitIdle(queue_);
    d().vkFreeCommandBuffers(device_, pool_, 1, &cmd);

    std::vector<uint8_t> out(out_size);
    void* p = nullptr;
    d().vkMapMemory(device_, readback_mem, 0, out_size, 0, &p);
    std::memcpy(out.data(), p, out_size);
    d().vkUnmapMemory(device_, readback_mem);
    compositors_.push_back(std::move(comp));  // outlives its in-flight frame
    return out;
  }

  VkInstance instance_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_{VK_NULL_HANDLE};
  uint32_t family_{0};
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue queue_{VK_NULL_HANDLE};
  VkCommandPool pool_{VK_NULL_HANDLE};
  std::vector<std::pair<VkImage, VkDeviceMemory>> images_;
  std::vector<VkImageView> views_;
  std::vector<std::pair<VkBuffer, VkDeviceMemory>> buffers_;
  std::vector<std::unique_ptr<wl_vulkan::LayerCompositor>> compositors_;
};

}  // namespace

// For each of the eight transforms: lay the image out as a producer would,
// draw it through the transform, and get the image back pixel for pixel.
TEST_F(LayerDrawVk, EveryTransformDrawsTheImageUpright) {
  for (uint32_t ti = 0; ti < 8; ++ti) {
    const auto t = static_cast<BufferTransform>(ti);
    SCOPED_TRACE(ti);
    const bool swap = TransformSwapsAxes(t);
    const int bw = swap ? kH : kW;
    const int bh = swap ? kW : kH;
    std::vector<uint8_t> buffer(Px(0, bh, bw), 0);
    for (int y = 0; y < kH; ++y) {
      for (int x = 0; x < kW; ++x) {
        const auto [bx, by] = ProducerPlaces(t, x, y);
        const auto c = ColorOf(y * kW + x);
        std::memcpy(&buffer[Px(bx, by, bw)], c.data(), 4);
      }
    }
    const auto out = Draw(
        bw, bh, buffer,
        UvForLayer({}, static_cast<uint32_t>(bw), static_cast<uint32_t>(bh), t),
        /*opaque=*/false);
    ASSERT_EQ(out.size(), static_cast<size_t>(kW) * kH * 4);
    for (int i = 0; i < kW * kH; ++i) {
      const auto want = ColorOf(i);
      const uint8_t* got = &out[static_cast<size_t>(i) * 4];
      EXPECT_TRUE(std::memcmp(got, want.data(), 4) == 0)
          << "destination (" << i % kW << ", " << i / kW << "): got "
          << int(got[0]) << "," << int(got[1]) << "," << int(got[2]) << " want "
          << int(want[0]) << "," << int(want[1]) << "," << int(want[2]);
    }
  }
}

TEST_F(LayerDrawVk, CropSelectsTheSourceRegion) {
  std::vector<uint8_t> buffer(static_cast<size_t>(2) * kW * kH * 4, 0);
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      const auto c = ColorOf(y * kW + x);
      std::memcpy(&buffer[Px(kW + x, y, 2 * kW)], c.data(), 4);
    }
  }
  const auto out =
      Draw(2 * kW, kH, buffer,
           UvForLayer({kW, 0, kW, kH}, 2 * kW, kH, BufferTransform::kNormal),
           /*opaque=*/false);
  ASSERT_EQ(out.size(), static_cast<size_t>(kW) * kH * 4);
  for (int i = 0; i < kW * kH; ++i) {
    EXPECT_TRUE(std::memcmp(&out[static_cast<size_t>(i) * 4], ColorOf(i).data(),
                            4) == 0)
        << "pixel " << i;
  }
}

// Over a white backdrop: an opaque layer covers it (its alpha channel, as an
// XRGB buffer's is, is ignored); the same pixels drawn translucent let the
// backdrop through.
TEST_F(LayerDrawVk, OpaqueIgnoresTheBufferAlpha) {
  std::vector<uint8_t> buffer(static_cast<size_t>(kW) * kH * 4, 0);
  for (int i = 0; i < kW * kH; ++i) {
    buffer[static_cast<size_t>(i) * 4] = 0x40;
    buffer[static_cast<size_t>(i) * 4 + 3] = 0x10;  // undefined in XRGB
  }
  const std::array<float, 4> white = {1, 1, 1, 1};
  const auto opaque = Draw(kW, kH, buffer, UvAffine{}, true, &white);
  const auto translucent = Draw(kW, kH, buffer, UvAffine{}, false, &white);
  ASSERT_EQ(opaque.size(), static_cast<size_t>(kW) * kH * 4);
  ASSERT_EQ(translucent.size(), opaque.size());
  for (int i = 0; i < kW * kH; ++i) {
    const auto px = static_cast<size_t>(i) * 4;
    EXPECT_EQ(opaque[px], 0x40) << "pixel " << i;           // red: the layer
    EXPECT_EQ(opaque[px + 1], 0x00) << "pixel " << i;       // green: no white
    EXPECT_GT(translucent[px + 1], 0xe0) << "pixel " << i;  // white shows
  }
}
