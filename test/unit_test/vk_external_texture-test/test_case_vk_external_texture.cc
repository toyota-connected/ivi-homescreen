// Copyright 2026 Toyota Connected North America
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <thread>

#include <gtest/gtest.h>
#include <vulkan/vulkan_core.h>

#include "flutter_desktop_texture_registrar.h"
#include "flutter_desktop_vk_texture.h"

namespace {

using namespace std::chrono_literals;

struct Producer {
  FlutterDesktopVulkanImage image{sizeof(FlutterDesktopVulkanImage), 0,
                                  VK_FORMAT_R8G8B8A8_UNORM};
  FlutterDesktopGpuSurfaceDescriptor descriptor{};
  bool return_null = false;
  std::atomic<int> calls{0};
  std::atomic<int> releases{0};
  size_t requested_width = 0;
  size_t requested_height = 0;
  // Optional hook run inside the GPU-surface callback.
  std::function<void()> during_callback;

  Producer() {
    descriptor.struct_size = sizeof(FlutterDesktopGpuSurfaceDescriptor);
    descriptor.handle = &image;
    descriptor.release_callback = &Producer::Release;
    descriptor.release_context = this;
  }

  static const FlutterDesktopGpuSurfaceDescriptor* Callback(size_t width,
                                                            size_t height,
                                                            void* user_data) {
    auto* self = static_cast<Producer*>(user_data);
    ++self->calls;
    self->requested_width = width;
    self->requested_height = height;
    if (self->during_callback) {
      self->during_callback();
    }
    return self->return_null ? nullptr : &self->descriptor;
  }

  static void Release(void* user_data) {
    ++static_cast<Producer*>(user_data)->releases;
  }
};

struct Completion {
  std::atomic<int> count{0};
  static void Fire(void* user_data) {
    ++static_cast<Completion*>(user_data)->count;
  }
};

class VkExternalTextureTest : public ::testing::Test {
 public:
  static constexpr int64_t kTestId = (1LL << 48) + 7;
  FlutterDesktopTextureRegistrar& registrar() { return registrar_; }

 protected:
  static constexpr int64_t kId = kTestId;

  void SetUp() override {
    registrar_.engine = nullptr;
    texture_ = std::make_shared<VkImageTexture>();
    texture_->callback = &Producer::Callback;
    texture_->user_data = &producer_;
    texture_->max_image_dimension = 16384;
    auto desc = std::make_unique<GL_TEXTURE_2D_DESC>();
    desc->vk_image = texture_;
    registrar_.texture_registry[kId] = std::move(desc);
  }

  void TearDown() override {
    // Every frame a test resolved must have been released by it.
    EXPECT_EQ(OutstandingVkImageFramesForTesting(), 0u);
  }

  static FlutterVulkanExternalTexture EngineTexture() {
    FlutterVulkanExternalTexture texture{};
    texture.struct_size = sizeof(FlutterVulkanExternalTexture);
    return texture;
  }

  bool Resolve(FlutterVulkanExternalTexture* out,
               size_t width = 0,
               size_t height = 0) {
    return PopulateExternalVulkanTextureFrame(&registrar_, kId, width, height,
                                              out);
  }

  // What the engine's unregister does to a registered texture: drop the map
  // entry, then retire it. Returns RetireVkImageTexture's result.
  bool Unregister(Completion* done) {
    std::shared_ptr<VkImageTexture> removed;
    {
      std::scoped_lock lock(registrar_.texture_mutex);
      removed = registrar_.texture_registry[kId]->vk_image;
      registrar_.texture_registry.erase(kId);
    }
    return done ? RetireVkImageTexture(removed, &Completion::Fire, done)
                : RetireVkImageTexture(removed, nullptr, nullptr);
  }

  FlutterDesktopTextureRegistrar registrar_;
  std::shared_ptr<VkImageTexture> texture_;
  Producer producer_;
};

TEST_F(VkExternalTextureTest, ResolvesTheImageThePluginNames) {
  producer_.image.image = 0xC0FFEE;
  producer_.descriptor.width = 1920;
  producer_.descriptor.height = 1080;

  auto out = EngineTexture();
  ASSERT_TRUE(Resolve(&out, 640, 480));

  EXPECT_EQ(producer_.calls, 1);
  EXPECT_EQ(producer_.requested_width, 640u);
  EXPECT_EQ(producer_.requested_height, 480u);
  EXPECT_EQ(out.struct_size, sizeof(FlutterVulkanExternalTexture));
  EXPECT_EQ(out.image, 0xC0FFEEu);
  EXPECT_EQ(out.format, static_cast<uint32_t>(VK_FORMAT_R8G8B8A8_UNORM));
  EXPECT_EQ(out.width, 1920u);
  EXPECT_EQ(out.height, 1080u);
  ASSERT_NE(out.destruction_callback, nullptr);
  // The engine gets an embedder token, not the plugin's context.
  EXPECT_NE(out.user_data, &producer_);

  EXPECT_EQ(producer_.releases, 0);
  out.destruction_callback(out.user_data);
  EXPECT_EQ(producer_.releases, 1);
}

TEST_F(VkExternalTextureTest, SecondEngineReleaseOfAFrameIsIgnored) {
  producer_.image.image = 1;
  auto out = EngineTexture();
  ASSERT_TRUE(Resolve(&out));
  out.destruction_callback(out.user_data);
  out.destruction_callback(out.user_data);  // Skia-path double report
  EXPECT_EQ(producer_.releases, 1);
}

TEST_F(VkExternalTextureTest, CallsThePluginOnEveryResolve) {
  producer_.image.format = VK_FORMAT_B8G8R8A8_UNORM;
  for (uint64_t i = 1; i <= 3; ++i) {
    producer_.image.image = i;
    auto out = EngineTexture();
    ASSERT_TRUE(Resolve(&out));
    EXPECT_EQ(out.image, i);
    out.destruction_callback(out.user_data);
  }
  EXPECT_EQ(producer_.calls, 3);
  EXPECT_EQ(producer_.releases, 3);
}

TEST_F(VkExternalTextureTest, ZeroSizeLeavesSizingToTheEngine) {
  producer_.image.image = 1;
  auto out = EngineTexture();
  ASSERT_TRUE(Resolve(&out, 320, 240));
  EXPECT_EQ(out.width, 0u);
  EXPECT_EQ(out.height, 0u);
  out.destruction_callback(out.user_data);
}

TEST_F(VkExternalTextureTest, UnknownTextureIdIsRejected) {
  auto out = EngineTexture();
  EXPECT_FALSE(
      PopulateExternalVulkanTextureFrame(&registrar_, kId + 1, 0, 0, &out));
  EXPECT_EQ(producer_.calls, 0);
}

TEST_F(VkExternalTextureTest, NoFrameFromThePluginIsRejected) {
  producer_.return_null = true;
  auto out = EngineTexture();
  EXPECT_FALSE(Resolve(&out));
  EXPECT_EQ(producer_.calls, 1);
  EXPECT_EQ(producer_.releases, 0);
}

TEST_F(VkExternalTextureTest, UnusableDescriptorIsReleasedAndRejected) {
  // A null VkImage.
  auto out = EngineTexture();
  EXPECT_FALSE(Resolve(&out));
  EXPECT_EQ(producer_.releases, 1);

  // A missing image struct.
  producer_.descriptor.handle = nullptr;
  EXPECT_FALSE(Resolve(&out));
  EXPECT_EQ(producer_.releases, 2);

  // An image struct from a different header revision.
  producer_.descriptor.handle = &producer_.image;
  producer_.image.image = 1;
  producer_.image.struct_size = sizeof(FlutterDesktopVulkanImage) - 1;
  EXPECT_FALSE(Resolve(&out));
  EXPECT_EQ(producer_.releases, 3);
  EXPECT_EQ(out.image, 0u);
}

TEST_F(VkExternalTextureTest, FormatsTheEngineCannotSampleAreRejected) {
  producer_.image.image = 1;
  auto out = EngineTexture();
  for (const VkFormat format :
       {VK_FORMAT_UNDEFINED, VK_FORMAT_B8G8R8A8_SRGB,  // Impeller-only
        VK_FORMAT_D24_UNORM_S8_UINT, static_cast<VkFormat>(0x7fffffff)}) {
    producer_.image.format = format;
    EXPECT_FALSE(Resolve(&out)) << format;
  }
  EXPECT_EQ(producer_.releases, 4);
}

TEST_F(VkExternalTextureTest, YcbcrNeedsTheSamplerConversionFeature) {
  producer_.image.image = 1;
  producer_.image.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;  // NV12
  auto out = EngineTexture();
  EXPECT_FALSE(Resolve(&out));
  EXPECT_EQ(producer_.releases, 1);

  texture_->allow_ycbcr = true;
  ASSERT_TRUE(Resolve(&out));
  EXPECT_EQ(out.format,
            static_cast<uint32_t>(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM));
  out.destruction_callback(out.user_data);
}

TEST_F(VkExternalTextureTest, SizesBeyondTheDeviceLimitAreRejected) {
  producer_.image.image = 1;
  producer_.descriptor.width = 16385;
  producer_.descriptor.height = 16;
  auto out = EngineTexture();
  EXPECT_FALSE(Resolve(&out));
  EXPECT_EQ(producer_.releases, 1);
}

TEST_F(VkExternalTextureTest, ShorterEngineStructIsNotOverrun) {
  producer_.image.image = 0xBEEF;
  producer_.descriptor.width = 64;
  producer_.descriptor.height = 64;

  // An engine whose FlutterVulkanExternalTexture ends before |width|.
  constexpr size_t kSentinel = 0xA5A5A5A5;
  auto out = EngineTexture();
  out.struct_size = offsetof(FlutterVulkanExternalTexture, width);
  out.width = kSentinel;
  out.height = kSentinel;
  ASSERT_TRUE(Resolve(&out));
  EXPECT_EQ(out.image, 0xBEEFu);
  EXPECT_EQ(out.width, kSentinel);
  EXPECT_EQ(out.height, kSentinel);
  out.destruction_callback(out.user_data);
}

TEST_F(VkExternalTextureTest, EngineStructWithoutReleaseIsRefused) {
  producer_.image.image = 1;
  auto out = EngineTexture();
  out.struct_size = offsetof(FlutterVulkanExternalTexture, user_data);
  EXPECT_FALSE(Resolve(&out));
  EXPECT_EQ(producer_.calls, 0);
}

TEST_F(VkExternalTextureTest, NothingResolvesWhileShuttingDown) {
  producer_.image.image = 1;
  registrar_.shutting_down.store(true);
  auto out = EngineTexture();
  EXPECT_FALSE(Resolve(&out));
  EXPECT_EQ(producer_.calls, 0);
}

TEST_F(VkExternalTextureTest, PluginRunsWithoutTheRegistrarLock) {
  producer_.image.image = 1;
  bool lock_was_free = false;
  producer_.during_callback = [&] {
    lock_was_free = registrar_.texture_mutex.try_lock();
    if (lock_was_free) {
      registrar_.texture_mutex.unlock();
    }
  };
  auto out = EngineTexture();
  ASSERT_TRUE(Resolve(&out));
  EXPECT_TRUE(lock_was_free);
  out.destruction_callback(out.user_data);
}

TEST_F(VkExternalTextureTest, UnregisterCompletesOnlyAfterTheEngineReleases) {
  producer_.image.image = 1;
  auto out = EngineTexture();
  ASSERT_TRUE(Resolve(&out));

  Completion done;
  Unregister(&done);
  // The engine still holds the frame: the plugin must not free it yet.
  EXPECT_EQ(done.count, 0);
  EXPECT_EQ(producer_.releases, 0);

  out.destruction_callback(out.user_data);
  EXPECT_EQ(producer_.releases, 1);
  EXPECT_EQ(done.count, 1);
}

TEST_F(VkExternalTextureTest, UnregisterWithNothingOutstandingCompletesNow) {
  Completion done;
  Unregister(&done);
  EXPECT_EQ(done.count, 1);

  auto out = EngineTexture();
  EXPECT_FALSE(Resolve(&out));
}

TEST_F(VkExternalTextureTest, RetiredTextureIsNeverResolvedAgain) {
  producer_.image.image = 1;
  // A resolve that raced the unregister and already holds the texture.
  Completion done;
  RetireVkImageTexture(texture_, &Completion::Fire, &done);
  auto out = EngineTexture();
  EXPECT_FALSE(ResolveVkImageTexture(texture_, 0, 0, &out));
  EXPECT_EQ(producer_.calls, 0);
  EXPECT_EQ(done.count, 1);
}

// Drives a resolve that parks inside the plugin callback on a "raster"
// thread until released.
class ParkedResolve {
 public:
  explicit ParkedResolve(VkExternalTextureTest* test, Producer* producer)
      : proceed_future_(proceed_.get_future().share()) {
    producer->during_callback = [this] {
      entered_.set_value();
      proceed_future_.wait();
    };
    raster_ = std::thread([test, this] {
      auto out = FlutterVulkanExternalTexture{};
      out.struct_size = sizeof(out);
      resolved_ = PopulateExternalVulkanTextureFrame(
          &test->registrar(), VkExternalTextureTest::kTestId, 0, 0, &out);
    });
    entered_.get_future().wait();
  }
  void Release() {
    proceed_.set_value();
    raster_.join();
  }
  bool resolved() const { return resolved_; }

 private:
  std::promise<void> entered_;
  std::promise<void> proceed_;
  std::shared_future<void> proceed_future_;
  std::thread raster_;
  bool resolved_ = true;
};

TEST_F(VkExternalTextureTest, UnregisterDoesNotWaitOnAResolveInFlight) {
  producer_.image.image = 1;
  ParkedResolve resolve(this, &producer_);

  // A producer thread unregistering while the raster thread waits on it
  // must not block, or the two would deadlock.
  Completion done;
  auto unregistered =
      std::async(std::launch::async, [&] { return Unregister(&done); });
  ASSERT_EQ(unregistered.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(unregistered.get());
  // ...but completion waits for the plugin callback to return.
  EXPECT_EQ(done.count, 0);

  resolve.Release();
  EXPECT_FALSE(resolve.resolved());
  // The frame produced during the unregister was handed straight back.
  EXPECT_EQ(producer_.releases, 1);
  EXPECT_EQ(done.count, 1);
}

TEST_F(VkExternalTextureTest, UnregisterWithoutCompletionWaitsForTheCallback) {
  producer_.image.image = 1;
  ParkedResolve resolve(this, &producer_);

  auto unregistered =
      std::async(std::launch::async, [&] { return Unregister(nullptr); });
  // Returning is the plugin's only signal, so it may not happen while the
  // plugin callback is still running.
  EXPECT_EQ(unregistered.wait_for(50ms), std::future_status::timeout);

  resolve.Release();
  EXPECT_TRUE(unregistered.get());
  EXPECT_EQ(producer_.releases, 1);
}

TEST_F(VkExternalTextureTest, UnregisterWithoutCompletionReportsHeldFrames) {
  producer_.image.image = 1;
  auto out = EngineTexture();
  ASSERT_TRUE(Resolve(&out));
  EXPECT_FALSE(Unregister(nullptr));
  out.destruction_callback(out.user_data);
  EXPECT_EQ(producer_.releases, 1);
}

TEST_F(VkExternalTextureTest, UnregisterFromInsideTheCallbackDoesNotDeadlock) {
  producer_.image.image = 1;
  Completion done;
  producer_.during_callback = [&] { Unregister(&done); };

  auto out = EngineTexture();
  EXPECT_FALSE(Resolve(&out));
  EXPECT_EQ(producer_.releases, 1);
  EXPECT_EQ(done.count, 1);
}

}  // namespace
