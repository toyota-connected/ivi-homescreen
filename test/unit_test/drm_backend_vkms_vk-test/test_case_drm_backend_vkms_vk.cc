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

// A real VulkanDrmBackend on a vkms card, driven the way the engine drives it.
//
// The backend had no test at all before this: it refuses a CPU/software Vulkan
// device, and a CI runner has no GPU, so nothing could bring it up.
// IVI_DRMVK_ALLOW_SOFTWARE=1 lifts that refusal, and llvmpipe then does real
// zero-copy scanout on vkms -- dma-buf exported from the ICD, imported as a KMS
// framebuffer, flipped on a plane.
//
// The fixture stands in for the engine: it takes a backing store through the
// compositor callback, draws nothing into it, and presents it. That is enough
// to drive a real commit and a real page flip, which is what every ordering
// question about this backend needs.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "backend/drm_kms_vulkan/vulkan_drm_backend.h"
#include "logging/logger.hpp"
#include "platform/homescreen/platform_views/dmabuf_vulkan_import.h"
#include "task_runner.h"

extern "C" {
#include <dirent.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
}

namespace {

constexpr const char* kAllowSoftware = "IVI_DRMVK_ALLOW_SOFTWARE";

struct VkmsCard {
  std::string path;
  uint32_t mode_w{0};
  uint32_t mode_h{0};
  int connectors{0};
  int connected{0};

  [[nodiscard]] bool ok() const { return !path.empty() && mode_w != 0; }
};

// Same discovery as the EGL vkms fixture: match drmGetVersion, then take the
// first connected connector that has a mode. Duplicated rather than shared
// because the two fixtures are separate binaries with no common header.
VkmsCard FindVkms() {
  VkmsCard out;
  for (int i = 0; i < 8; ++i) {
    const std::string path = "/dev/dri/card" + std::to_string(i);
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      continue;
    }
    drmVersionPtr v = drmGetVersion(fd);
    const bool is_vkms =
        v != nullptr && v->name != nullptr && std::string(v->name) == "vkms";
    if (v != nullptr) {
      drmFreeVersion(v);
    }
    if (!is_vkms) {
      ::close(fd);
      continue;
    }
    out.path = path;
    if (drmModeRes* res = drmModeGetResources(fd); res != nullptr) {
      for (int c = 0; c < res->count_connectors; ++c) {
        drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[c]);
        if (conn == nullptr) {
          continue;
        }
        out.connectors++;
        if (conn->connection == DRM_MODE_CONNECTED) {
          out.connected++;
          if (out.mode_w == 0 && conn->count_modes > 0) {
            out.mode_w = conn->modes[0].hdisplay;
            out.mode_h = conn->modes[0].vdisplay;
          }
        }
        drmModeFreeConnector(conn);
      }
      drmModeFreeResources(res);
    }
    ::close(fd);
    if (out.ok()) {
      break;
    }
  }
  return out;
}

class VulkanDrmVkms : public ::testing::Test {
 protected:
  void SetUp() override {
    card_ = FindVkms();
    if (card_.path.empty()) {
      GTEST_SKIP() << "no vkms card on this host (sudo modprobe vkms)";
    }
    if (!card_.ok()) {
      GTEST_SKIP() << card_.path << " is vkms but exposes no connected "
                   << "connector with modes (connectors=" << card_.connectors
                   << " connected=" << card_.connected << ")";
    }

    // Before Create: device selection reads this, and without it the backend
    // refuses every CPU device -- which on a runner is the only kind there is.
    if (const char* prev = std::getenv(kAllowSoftware); prev != nullptr) {
      saved_allow_ = prev;
    }
    ::setenv(kAllowSoftware, "1", 1);

    // No DrmDisplay here, deliberately. This backend opens the card and
    // acquires DRM master itself; a DrmDisplay would take master first and
    // Create would then refuse with EACCES against our own handle -- which
    // reads exactly like "another display server holds this card".
    // No session and no shared device: the backend opens the card and takes
    // master itself, which is the --drm-no-seat arrangement. Handing it a
    // DrmDisplay's device instead would mean master was already held, and
    // Create would refuse against our own handle.
    backend_ = VulkanDrmBackend::Create(
        card_.path, /*enable_validation=*/false, /*session=*/nullptr,
        /*shared_device=*/nullptr, /*mode_spec=*/"", /*connector_name=*/"",
        /*rotation=*/0);
    if (backend_ == nullptr) {
      // No Vulkan loader, or no device that can import dma-buf at all. A skip,
      // not a failure: the host simply cannot run this backend.
      GTEST_SKIP() << "VulkanDrmBackend::Create refused on " << card_.path
                   << " (no usable Vulkan device for zero-copy scanout)";
    }
  }

  void TearDown() override {
    backend_.reset();
    if (saved_allow_) {
      ::setenv(kAllowSoftware, saved_allow_->c_str(), 1);
    } else {
      ::unsetenv(kAllowSoftware);
    }
  }

  // One frame: take a store, present it as the single layer. The engine would
  // render into it first; nothing here needs its contents, only that a real
  // buffer reaches a real commit.
  bool PresentOneFrame() {
    FlutterBackingStoreConfig cfg{};
    cfg.struct_size = sizeof(FlutterBackingStoreConfig);
    cfg.size = FlutterSize{static_cast<double>(card_.mode_w),
                           static_cast<double>(card_.mode_h)};
    FlutterBackingStore bs{};
    if (!backend_->CreateBackingStoreForTest(&cfg, &bs)) {
      return false;
    }

    FlutterLayer layer{};
    layer.struct_size = sizeof(FlutterLayer);
    layer.type = kFlutterLayerContentTypeBackingStore;
    layer.backing_store = &bs;
    layer.offset = FlutterPoint{0.0, 0.0};
    layer.size = cfg.size;
    const FlutterLayer* layers[] = {&layer};

    const bool ok = backend_->PresentLayersForTest(layers, 1);
    backend_->CollectBackingStoreForTest(&bs);
    return ok;
  }

  VkmsCard card_;
  std::shared_ptr<VulkanDrmBackend> backend_;

 private:
  std::optional<std::string> saved_allow_;
};

}  // namespace

// The bring-up this fixture exists to make possible. Before
// IVI_DRMVK_ALLOW_SOFTWARE there was no way to reach this line without a GPU.
TEST_F(VulkanDrmVkms, TheBackendComesUpOnVkms) {
  ASSERT_NE(backend_, nullptr);
  EXPECT_GT(backend_->width(), 0u);
  EXPECT_GT(backend_->height(), 0u);
}

// A real commit and a real flip, which every ordering question about this
// backend needs -- the presentation serial (#662) cannot be asserted from a
// seam, only from a frame that actually reached the display.
TEST_F(VulkanDrmVkms, APresentedFrameReachesTheDisplay) {
  ASSERT_TRUE(PresentOneFrame()) << "the first present is the blocking modeset";
  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(PresentOneFrame()) << "frame " << i << " was not presented";
  }
  // Not the return value alone: the backend's own commit count, so a present
  // that succeeded without reaching a commit cannot pass this.
  EXPECT_GE(backend_->PresentedFramesForTest(), 5u)
      << "five presents returned true but the backend committed fewer frames";
}

// The serial a flip event will report must already name this frame by the time
// the commit is in flight.
//
// OnFlipEvent reads c.presentation_serial to say which frame a flip showed.
// That serial used to be published after the commit returned, and every commit
// here is NONBLOCK -- so an event dispatched before the raster thread got back
// named serial-1, a frame already reported, while the frame on screen went
// unreported. The skew is permanent: each commit adds one serial and each event
// consumes one.
//
// on_commit_returned_ runs on the raster thread with the flip in flight, which
// is precisely where the event would read it. Asserting there needs no producer
// and no real flip -- the value at that instant is the whole question.
TEST_F(VulkanDrmVkms, TheSerialNamesThisFrameWhileItsCommitIsInFlight) {
  ASSERT_TRUE(PresentOneFrame()) << "the first present is the blocking modeset";

  const uint64_t before = backend_->PresentationSerialForTest();
  ASSERT_GT(before, 0u) << "no serial was ever staged, so this case cannot run";

  uint64_t seen_in_flight = 0;
  int hook_runs = 0;
  backend_->on_commit_returned_ = [this, &seen_in_flight, &hook_runs] {
    ++hook_runs;
    seen_in_flight = backend_->PresentationSerialForTest();
  };
  const bool presented = PresentOneFrame();
  backend_->on_commit_returned_ = nullptr;
  ASSERT_TRUE(presented);
  ASSERT_EQ(hook_runs, 1) << "the hook never ran, so nothing was observed";

  EXPECT_EQ(seen_in_flight, before + 1)
      << "while this frame's commit was in flight the serial still named the "
         "previous frame, so a flip event dispatched then would report the "
         "wrong "
         "one -- and every later flip would too";
}

// A VT switch must be survivable: master goes away, comes back, and the display
// has to return.
//
// This backend registered no libseat pause/resume handlers at all -- it stored
// the session and never used it -- so it kept committing into a revoked fd, its
// layer sources kept fd-bound state a resume invalidates, and nothing
// re-established the mode on the way back. The EGL backend has had this since
// drm_backend.cc:319; this is the Vulkan half.
//
// Driven directly rather than through libseat: the fixture has no seat (the
// backend self-acquires master), and what matters is the state either side of
// the callbacks, not who invoked them.
TEST_F(VulkanDrmVkms, AVtSwitchLeavesTheBackendAbleToPresent) {
  ASSERT_TRUE(PresentOneFrame()) << "the first present is the blocking modeset";
  ASSERT_TRUE(PresentOneFrame()) << "and a steady-state flip after it";
  const uint64_t before = backend_->PresentedFramesForTest();

  // Force it up rather than hoping the last flip's event has not landed yet:
  // otherwise the assertion below passes whether pause clears the latch or not.
  backend_->SetFlipPendingForTest(true);
  ASSERT_TRUE(backend_->FlipPendingForTest());

  backend_->OnSessionPaused();
  EXPECT_TRUE(backend_->SessionPaused());
  EXPECT_FALSE(backend_->FlipPendingForTest())
      << "no flip event is coming for a commit made before the revoke, so the "
         "latch it raised must not stand";

  backend_->OnSessionResumed(backend_->DrmFdForTest());
  EXPECT_FALSE(backend_->SessionPaused());
  EXPECT_FALSE(backend_->FlipPendingForTest());

  // The real assertion: frames again. Without a resume that puts first_commit
  // back, the next commit is a plain non-blocking flip against a CRTC whose
  // mode went away with master.
  EXPECT_TRUE(PresentOneFrame()) << "nothing presented after the switch back";
  EXPECT_GT(backend_->PresentedFramesForTest(), before)
      << "the backend returned true but committed nothing after resume";
}

// Resume has to release the plane-layer path's slots too, not just the
// single-plane indexes.
//
// The plane path tracks its live stores in plane_scanning_slots and
// plane_pending_slots rather than scanning_slot/pending_slot, and
// AcquireScanoutSlot excludes all four when hunting for a reusable slot.
// Resume used to clear only the two indexes, so after a VT switch the plane
// lists still reserved slots nothing was scanning out -- with the ring full,
// every CreateBackingStoreImpl afterwards failed with "scanout ring
// exhausted" rather than reusing a released buffer.
//
// The lists are seeded directly: what is under test is the clearing, not the
// plane path that fills them, and the fixture's vkms has no second plane to
// commit a real plane frame onto.
TEST_F(VulkanDrmVkms, AVtSwitchReleasesThePlaneLayerSlots) {
  ASSERT_TRUE(PresentOneFrame()) << "the first present is the blocking modeset";

  backend_->SetPlaneSlotsHeldForTest(3, 2);
  ASSERT_EQ(backend_->PlaneSlotsHeldForTest(), 5U)
      << "the fixture did not take the seeded slots";

  backend_->OnSessionPaused();
  backend_->OnSessionResumed(backend_->DrmFdForTest());

  EXPECT_EQ(backend_->PlaneSlotsHeldForTest(), 0U)
      << "master loss ended that scanout, so resume must hand the slots back; "
         "left populated they reserve buffers nothing is using";
  EXPECT_TRUE(PresentOneFrame()) << "nothing presented after the switch back";
}

// ─── Stall detector (#660) ─────────────────────────────────────────────────
//
// The EGL backend got this in #659; this one can stall the same way and was
// not covered. A parked baton means Flutter is waiting for the flip event of
// the commit in flight. If that event is lost the baton never comes back, no
// frame is built, and because no frame is built the present path -- and so its
// own 100 ms spin guard -- never runs again. The display stops until a VT
// switch. The detector watches from the task runner's io_context, which is
// alive exactly then.
//
// Two ticks with no progress is the signal, so one tick must not fire: a baton
// parked for less than the threshold is just a frame in flight.
TEST_F(VulkanDrmVkms, AStalledFlipIsRecoveredNotWaitedOnForever) {
  ASSERT_TRUE(PresentOneFrame()) << "the first present is the blocking modeset";

  // Delivery needs a runner: PostOnVsync marshals onto it, and without one the
  // provider leaves the baton parked whatever the detector decides. Wired on
  // the provider directly rather than through SetPlatformTaskRunner, which
  // would also start the real timer on an io_context nothing here runs.
  FLUTTER_API_SYMBOL(FlutterEngine) no_engine{nullptr};
  TaskRunner runner("vk-stall-test", no_engine);
  backend_->VsyncForTest().SetEngine(nullptr, &runner);

  // The state the detector exists for: a commit in flight whose event will
  // never arrive, and Flutter waiting on it. Both latches, because that is
  // what a real commit raises.
  backend_->SetFlipPendingForTest(true);
  backend_->VsyncForTest().SetSourcePending(true);
  backend_->VsyncForTest().SubmitBaton(nullptr, 0x6060);
  ASSERT_TRUE(backend_->VsyncForTest().HasParkedBaton())
      << "the latch is up, so the baton must park -- otherwise there is no "
         "stall to detect and this case proves nothing";

  const uint64_t before = backend_->StallRecoveriesForTest();
  backend_->CheckForStallForTest();
  EXPECT_TRUE(backend_->VsyncForTest().HasParkedBaton())
      << "one tick is not evidence of a stall; a frame may simply be in flight";
  EXPECT_EQ(backend_->StallRecoveriesForTest(), before);

  // Second tick, nothing moved in between.
  backend_->CheckForStallForTest();
  EXPECT_FALSE(backend_->FlipPendingForTest())
      << "the latch has to go, or the baton parks again immediately";
  EXPECT_FALSE(backend_->VsyncForTest().HasParkedBaton())
      << "the baton must be handed back; nothing else is going to";
  EXPECT_EQ(backend_->StallRecoveriesForTest(), before + 1);
}

// The trap this backend adds over the EGL one: it mirrors its latch into the
// provider, and SubmitBaton gates on the provider's copy. Clearing only
// flip_pending hands *this* baton back and parks the next one forever -- a
// recovery that looks right in the log and leaves the display just as stuck.
// So the assertion is about the frame after the recovery, not the recovery.
TEST_F(VulkanDrmVkms, RecoveryClearsTheProviderLatchNotJustTheBackendOne) {
  ASSERT_TRUE(PresentOneFrame());

  FLUTTER_API_SYMBOL(FlutterEngine) no_engine{nullptr};
  TaskRunner runner("vk-stall-mirror-test", no_engine);
  backend_->VsyncForTest().SetEngine(nullptr, &runner);

  backend_->SetFlipPendingForTest(true);
  backend_->VsyncForTest().SetSourcePending(true);
  backend_->VsyncForTest().SubmitBaton(nullptr, 0x6161);
  ASSERT_TRUE(backend_->VsyncForTest().HasParkedBaton());

  backend_->CheckForStallForTest();
  backend_->CheckForStallForTest();
  ASSERT_EQ(backend_->StallRecoveriesForTest(), 1u)
      << "no recovery happened, so there is nothing to check here";

  // Flutter asks for the next frame. If the provider latch survived the
  // recovery this parks and the display is stalled again, one baton later.
  backend_->VsyncForTest().SubmitBaton(nullptr, 0x6262);
  EXPECT_FALSE(backend_->VsyncForTest().HasParkedBaton())
      << "the next baton parked, so the recovery cleared the backend latch and "
         "left the provider one up -- the display is still stalled";
}

// The other half: a display that is merely busy must never be recovered. Flip
// events advancing between ticks is progress, however long a baton has been
// parked. This is also what rules out keying on the commit counter, which
// advances during the stall itself.
TEST_F(VulkanDrmVkms, AFlipInFlightIsNotMistakenForAStall) {
  ASSERT_TRUE(PresentOneFrame()) << "the first present is the blocking modeset";

  const uint64_t before = backend_->StallRecoveriesForTest();
  const uint64_t flips_before = backend_->FlipsHandledForTest();
  for (int i = 0; i < 4; ++i) {
    backend_->CheckForStallForTest();
    ASSERT_TRUE(PresentOneFrame()) << "frame " << i;
  }
  EXPECT_EQ(backend_->StallRecoveriesForTest(), before)
      << "flips were advancing the whole time; nothing was stalled";
  EXPECT_GT(backend_->FlipsHandledForTest(), flips_before)
      << "no flip event was counted, so the progress signal the detector reads "
         "is dead and the case above passed for the wrong reason";
}

// A revoked session looks exactly like a lost flip event -- a baton parked, the
// latch up, no events coming -- and must not be reported as one.
TEST_F(VulkanDrmVkms, APausedSessionIsNotReportedAsAStall) {
  ASSERT_TRUE(PresentOneFrame());

  FLUTTER_API_SYMBOL(FlutterEngine) no_engine{nullptr};
  TaskRunner runner("vk-stall-pause-test", no_engine);
  backend_->VsyncForTest().SetEngine(nullptr, &runner);

  const uint64_t before = backend_->StallRecoveriesForTest();
  backend_->OnSessionPaused();
  // Put the latch back: OnSessionPaused drops it, and the case being guarded
  // is a commit whose event the revoke ate.
  backend_->SetFlipPendingForTest(true);
  backend_->VsyncForTest().SetSourcePending(true);
  backend_->VsyncForTest().SubmitBaton(nullptr, 0x6363);
  ASSERT_TRUE(backend_->VsyncForTest().HasParkedBaton())
      << "no baton parked, so there is no stall to mistake this for";

  for (int i = 0; i < 4; ++i) {
    backend_->CheckForStallForTest();
  }
  EXPECT_EQ(backend_->StallRecoveriesForTest(), before)
      << "a VT switch-out was reported as a lost flip event";
  EXPECT_TRUE(backend_->FlipPendingForTest())
      << "the latch is the resume's to clear while the session is revoked";

  backend_->OnSessionResumed(backend_->DrmFdForTest());
  EXPECT_FALSE(backend_->SessionPaused());
}

// What a plugin on the shared device gates its entry points on: the version
// usable on the device, which is not the physical device's own when the
// instance asked for less, and the instance extensions it may rely on.
TEST_F(VulkanDrmVkms, TheVulkanContextReportsTheUsableVersion) {
  BackendVulkanContext vk{};
  ASSERT_TRUE(backend_->GetVulkanContext(&vk));
  ASSERT_NE(vk.get_instance_proc_addr, nullptr);
  auto gipa =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(vk.get_instance_proc_addr);
  auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(
      static_cast<VkInstance>(vk.instance), "vkGetPhysicalDeviceProperties"));
  ASSERT_NE(props, nullptr);
  VkPhysicalDeviceProperties p{};
  props(static_cast<VkPhysicalDevice>(vk.physical_device), &p);

  EXPECT_NE(vk.api_version, 0u) << "the version went unreported";
  EXPECT_EQ(vk.api_version, std::min(p.apiVersion, VK_API_VERSION_1_1))
      << "the reported version is not the lower of the instance's and the "
         "physical device's";
  // This backend enables instance extensions only for validation or a
  // surface, so the list may be empty; what is listed must be there.
  if (vk.instance_extension_count > 0) {
    ASSERT_NE(vk.instance_extensions, nullptr);
  }
  for (size_t i = 0; i < vk.instance_extension_count; ++i) {
    EXPECT_NE(vk.instance_extensions[i], nullptr) << "entry " << i;
  }
}

#if BUILD_COMPOSITOR
// A platform view's imports are freed on the raster thread between frames,
// never on the thread that asks: the raster thread may be recording a frame
// that binds them (it crashed in vkCreateImageView when the free ran at once).
TEST_F(VulkanDrmVkms, ADeferredFreeWaitsForLaterPresents) {
  ASSERT_TRUE(PresentOneFrame()) << "the first present is the blocking modeset";
  std::atomic<int> runs{0};
  backend_->ScheduleDeferredDestroy([&] { runs.fetch_add(1); });
  EXPECT_EQ(runs.load(), 0) << "the free ran on the caller's thread at once";
  int presents = 0;
  while (runs.load() == 0 && presents < 12) {
    ASSERT_TRUE(PresentOneFrame());
    ++presents;
  }
  EXPECT_EQ(runs.load(), 1);
  EXPECT_GE(presents, 2) << "freed at the very next present: no margin for a "
                            "frame still in flight";
}

// One a present never reaped still runs, once the device is idle.
TEST_F(VulkanDrmVkms, AnUnreapedDeferredFreeRunsAtTeardown) {
  std::atomic<int> runs{0};
  backend_->ScheduleDeferredDestroy([&] { runs.fetch_add(1); });
  EXPECT_EQ(runs.load(), 0);
  backend_.reset();
  EXPECT_EQ(runs.load(), 1);
}
#endif

#if BUILD_COMPOSITOR
namespace {

// Fds this process has open on the dma-buf @ino.
int FdsOn(const ino_t ino) {
  int n = 0;
  DIR* d = opendir("/proc/self/fd");
  while (dirent* e = d != nullptr ? readdir(d) : nullptr) {
    struct stat st{};
    const std::string path = std::string("/proc/self/fd/") + e->d_name;
    if (stat(path.c_str(), &st) == 0 && st.st_ino == ino) {
      ++n;
    }
  }
  if (d != nullptr) {
    closedir(d);
  }
  return n;
}

// A LINEAR XRGB8888 buffer on the card, submitted the way a producer that pools
// its buffers submits it to each view: a fresh fd every time.
class PooledBuffer {
 public:
  bool Create(const int drm_fd, const uint32_t w, const uint32_t h) {
    gbm_ = gbm_create_device(drm_fd);
    if (gbm_ == nullptr) {
      return false;
    }
    bo_ = gbm_bo_create(gbm_, w, h, GBM_FORMAT_XRGB8888,
                        GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (bo_ == nullptr) {
      return false;
    }
    const int fd = gbm_bo_get_fd(bo_);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) {
      return false;
    }
    ino_ = st.st_ino;
    close(fd);
    return true;
  }
  ~PooledBuffer() {
    if (bo_ != nullptr) {
      gbm_bo_destroy(bo_);
    }
    if (gbm_ != nullptr) {
      gbm_device_destroy(gbm_);
    }
  }
  [[nodiscard]] IhsFrame Frame() const {
    IhsFrame f{};
    f.struct_size = sizeof(f);
    f.format.fourcc = DRM_FORMAT_XRGB8888;
    f.format.modifier = DRM_FORMAT_MOD_LINEAR;
    f.width = gbm_bo_get_width(bo_);
    f.height = gbm_bo_get_height(bo_);
    f.plane_count = 1;
    f.plane_fd[0] = gbm_bo_get_fd(bo_);
    f.plane_stride[0] = gbm_bo_get_stride(bo_);
    return f;
  }
  [[nodiscard]] ino_t ino() const { return ino_; }

 private:
  gbm_device* gbm_{nullptr};
  gbm_bo* bo_{nullptr};
  ino_t ino_{0};
};

}  // namespace

// A producer that pools its buffers submits the same dma-bufs to its next
// view. The import a closed view freed comes back rather than being made
// again, and draining the pool frees it.
TEST_F(VulkanDrmVkms, AFreedImportIsReusedForTheSameDmabuf) {
  BackendVulkanContext vk{};
  ASSERT_TRUE(backend_->GetVulkanContext(&vk));
  DmabufVulkanImporter importer;
  ASSERT_TRUE(importer.Init(static_cast<VkInstance>(vk.instance),
                            static_cast<VkPhysicalDevice>(vk.physical_device),
                            static_cast<VkDevice>(vk.device),
                            vk.get_instance_proc_addr));
  PooledBuffer buffer;
  ASSERT_TRUE(buffer.Create(backend_->DrmFdForTest(), 64, 64));
  const int base = FdsOn(buffer.ino());

  DmabufVulkanImporter::ImportedImage first;
  IhsFrame frame = buffer.Frame();
  if (!importer.Import(frame, &first)) {
    close(frame.plane_fd[0]);
    GTEST_SKIP() << "this device does not import a LINEAR dma-buf";
  }
  const VkImage image = first.image;
  importer.Destroy(&first);
  EXPECT_EQ(importer.PooledForTest(), 1u);

  DmabufVulkanImporter::ImportedImage second;
  ASSERT_TRUE(importer.Import(buffer.Frame(), &second));
  EXPECT_EQ(second.image, image) << "imported again rather than reused";
  EXPECT_EQ(importer.PooledForTest(), 0u);
  EXPECT_LE(FdsOn(buffer.ino()), base + 1)
      << "the submitted fd was kept beside the pooled import";

  importer.Destroy(&second);
  importer.DrainPool();
  EXPECT_EQ(importer.PooledForTest(), 0u);
  EXPECT_EQ(FdsOn(buffer.ino()), base) << "the drained import kept its fd";

  // Drained: a free is a free until the next Init.
  DmabufVulkanImporter::ImportedImage third;
  ASSERT_TRUE(importer.Import(buffer.Frame(), &third));
  importer.Destroy(&third);
  EXPECT_EQ(importer.PooledForTest(), 0u);
  EXPECT_EQ(FdsOn(buffer.ino()), base);
}

// Another layout of the same dma-buf is another image, and the pool is bounded.
TEST_F(VulkanDrmVkms, ThePoolMatchesTheLayoutAndStaysBounded) {
  BackendVulkanContext vk{};
  ASSERT_TRUE(backend_->GetVulkanContext(&vk));
  DmabufVulkanImporter importer;
  ASSERT_TRUE(importer.Init(static_cast<VkInstance>(vk.instance),
                            static_cast<VkPhysicalDevice>(vk.physical_device),
                            static_cast<VkDevice>(vk.device),
                            vk.get_instance_proc_addr));
  PooledBuffer buffer;
  ASSERT_TRUE(buffer.Create(backend_->DrmFdForTest(), 64, 64));

  DmabufVulkanImporter::ImportedImage full;
  IhsFrame frame = buffer.Frame();
  if (!importer.Import(frame, &full)) {
    close(frame.plane_fd[0]);
    GTEST_SKIP() << "this device does not import a LINEAR dma-buf";
  }
  importer.Destroy(&full);
  IhsFrame cropped = buffer.Frame();
  cropped.width = 32;
  DmabufVulkanImporter::ImportedImage part;
  ASSERT_TRUE(importer.Import(cropped, &part));
  EXPECT_EQ(part.width, 32u) << "handed back the 64-wide import";
  EXPECT_EQ(importer.PooledForTest(), 1u);
  importer.Destroy(&part);

  std::vector<std::unique_ptr<PooledBuffer>> more;
  for (size_t i = 0; i < DmabufVulkanImporter::kPooledImports + 4; ++i) {
    more.push_back(std::make_unique<PooledBuffer>());
    ASSERT_TRUE(more.back()->Create(backend_->DrmFdForTest(), 16, 16));
    DmabufVulkanImporter::ImportedImage img;
    ASSERT_TRUE(importer.Import(more.back()->Frame(), &img));
    importer.Destroy(&img);
  }
  EXPECT_EQ(importer.PooledForTest(), DmabufVulkanImporter::kPooledImports);
  EXPECT_EQ(FdsOn(buffer.ino()), 0) << "the oldest import was never freed";
  importer.DrainPool();
}
#endif

int main(int argc, char** argv) {
  IHS_LOGGING_START("TEST", "drm_kms_vulkan vkms test");
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
