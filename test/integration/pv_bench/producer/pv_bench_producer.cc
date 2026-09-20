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

/*
 * A platform-view producer that exists to be measured.
 *
 * It hands the registry a ring of dma-bufs carrying a moving pattern at the
 * display rate -- the shape a camera or a video decoder has, minus the decode.
 * That is enough to put a real platform-view layer in the scene, which is the
 * thing the compositor path exists for: with a compositor the layer is imported
 * and placed as its own layer, without one it has nowhere to go and the scene
 * is Flutter's alone.
 *
 * Deliberately CPU-filled. A GL or Vulkan producer would measure the producer's
 * renderer as much as the shell's composition, and the question here is what
 * the shell does with a layer, not how fast we can draw one.
 *
 * Load it over FFI and call pv_bench_register() from the platform thread; then
 * put up a platform view of type "pv_bench". See lib/main.dart.
 */

#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <drm_fourcc.h>

#include "ihs/platform_view.h"

namespace {

constexpr uint32_t kRingSlots = 3;
constexpr int kFenceWaitMs = 100;
constexpr auto kFramePeriod = std::chrono::microseconds(16667);

void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[pv_bench] ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  fflush(stderr);
  va_end(ap);
}

/*
 * Wait for a release fence, then close it. Bounded: a registry that stops
 * releasing must not park this thread forever, and a torn frame beats a hang
 * in a benchmark whose whole output is a frame rate.
 */
void WaitAndCloseFence(int fd) {
  if (fd < 0) {
    return;
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(kFenceWaitMs);
  pollfd pfd{fd, POLLIN, 0};
  for (;;) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const int timeout = left.count() > 0 ? static_cast<int>(left.count()) : 0;
    const int rc = poll(&pfd, 1, timeout);
    // Against a deadline, not a fresh interval per pass: restarting the full
    // timeout on each EINTR is how a bounded wait becomes unbounded.
    if (rc != -1 || errno != EINTR || timeout == 0) {
      break;
    }
  }
  ::close(fd);
}

/* One ring buffer: allocated once, mapped once, handed over many times. */
struct Slot {
  gbm_bo* bo = nullptr;
  int fd = -1;
  uint32_t stride = 0;     /* the dma-buf's stride, what the registry imports */
  uint32_t map_stride = 0; /* what the CPU mapping uses; usually the same */
  void* map = nullptr;
  void* map_data = nullptr;
  uint64_t modifier = DRM_FORMAT_MOD_INVALID;
  uint32_t planes = 1;
  int release_fence = -1;
  bool submitted = false;
};

class BenchView {
 public:
  BenchView(IhsPlatformView* view, uint32_t width, uint32_t height)
      : view_(view),
        width_(width ? width : 640),
        height_(height ? height : 360) {}

  ~BenchView() { Stop(); }

  bool Start() {
    /*
     * Ask before assuming. Which format+modifier pairs the backend can import
     * is a property of the GPU, not of the format: a device whose texture unit
     * cannot address a linear image imports nothing linear, however ordinary
     * XRGB8888 looks.
     */
    IhsPvCapabilities caps{};
    caps.struct_size = sizeof(caps);
    if (ihs_pv_query_capabilities(&caps) != IHS_PV_OK) {
      Log("capability query failed; nothing to negotiate against");
      return false;
    }
    Log("backend=%s kinds=0x%x explicit_sync=%u formats=%zu",
        caps.backend_key ? caps.backend_key : "?", caps.kinds,
        caps.explicit_sync, caps.format_count);

    /* Only the packed 32-bit RGB ones: this producer fills pixels on the CPU
     * and has no business emitting YUV. */
    std::vector<IhsFormatModifier> wanted;
    for (size_t i = 0; i < caps.format_count; ++i) {
      const IhsFormatModifier& f = caps.formats[i];
      if (f.fourcc == DRM_FORMAT_XRGB8888 || f.fourcc == DRM_FORMAT_ARGB8888 ||
          f.fourcc == DRM_FORMAT_XBGR8888 || f.fourcc == DRM_FORMAT_ABGR8888) {
        Log("offered fourcc %#x modifier %#llx", f.fourcc,
            static_cast<unsigned long long>(f.modifier));
        wanted.push_back(f);
      }
    }
    if (wanted.empty()) {
      Log("no packed RGB format offered; nothing this producer can fill");
      return false;
    }

    if (!OpenGbm()) {
      Log("no gbm device; nothing to submit");
      return false;
    }

    IhsPvRequirements req{};
    req.struct_size = sizeof(req);
    /*
     * Import is the kind the compositor path composes, and the one being
     * measured. The floor stays in the mask so a backend without import still
     * runs -- it just is not measuring the same path, and the granted-kind log
     * below says which one it got. DRM_PLANE is deliberately not asked for: it
     * is offered only on drm-kms-egl, so including it would silently measure a
     * different path on the EGL backend than on the Vulkan one.
     */
    req.kinds = IHS_PV_KIND_TEXTURE_DMABUF_IMPORT | IHS_PV_KIND_SOFTWARE_SHM;
    req.formats = wanted.data();
    req.format_count = wanted.size();
    req.needs_alpha = 0;
    req.sync = IHS_PV_SYNC_EXPLICIT_PREFERRED;
    req.z_order = IHS_PV_Z_INLINE;

    IhsPvGrant grant{};
    grant.struct_size = sizeof(grant);
    const int rc = ihs_pv_negotiate(view_, &req, &grant);
    if (rc != IHS_PV_OK) {
      Log("negotiate failed: %d", rc);
      return false;
    }
    Log("grant kind=0x%x sync=%u fourcc=%#x modifier %#llx for %ux%u",
        grant.granted_kind, grant.sync, grant.format.fourcc,
        static_cast<unsigned long long>(grant.format.modifier), width_,
        height_);
    format_ = grant.format;
    if (grant.granted_kind != IHS_PV_KIND_TEXTURE_DMABUF_IMPORT) {
      Log("not a dma-buf import grant: the plane path is NOT being measured");
    }

    if (!AllocRing()) {
      return false;
    }

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this] { Run(); });
    return true;
  }

  void Stop() {
    if (running_.exchange(false, std::memory_order_acq_rel) &&
        thread_.joinable()) {
      thread_.join();
    }
    for (Slot& s : slots_) {
      WaitAndCloseFence(s.release_fence);
      s.release_fence = -1;
      if (s.map) {
        gbm_bo_unmap(s.bo, s.map_data);
      }
      if (s.fd >= 0) {
        ::close(s.fd);
      }
      if (s.bo) {
        gbm_bo_destroy(s.bo);
      }
    }
    slots_.clear();
    if (gbm_) {
      gbm_device_destroy(gbm_);
      gbm_ = nullptr;
    }
    if (drm_fd_ >= 0) {
      ::close(drm_fd_);
      drm_fd_ = -1;
    }
  }

  /*
   * Size changes are logged and ignored. Reallocating mid-run would make the
   * numbers describe two different workloads; the app pins the view size.
   */
  void Resize(uint32_t w, uint32_t h) {
    if (w != width_ || h != height_) {
      Log("resize %ux%u ignored; still submitting %ux%u", w, h, width_,
          height_);
    }
  }

  void Suspend(bool suspended) {
    suspended_.store(suspended, std::memory_order_release);
    Log("%s", suspended ? "suspended" : "resumed");
  }

 private:
  bool OpenGbm() {
    /*
     * Which node the buffers come from decides whether the shell can import
     * them at all, and the answer is per-platform: a split display/render SoC
     * has one device that scans out and another that renders, and only one of
     * them allocates something the compositor's Vulkan device will take. So it
     * is selectable, with the render node first -- opening a card node while
     * the shell owns it is how a producer takes the display away from the thing
     * it is supposed to be feeding.
     */
    const char* forced = ::getenv("PV_BENCH_DRM_NODE");
    const char* candidates[] = {forced, "/dev/dri/renderD128",
                                "/dev/dri/renderD129"};
    for (const char* node : candidates) {
      if (node == nullptr) {
        continue;
      }
      if (forced != nullptr && node != forced) {
        break;  // an explicit node is a request, not a preference
      }
      drm_fd_ = ::open(node, O_RDWR | O_CLOEXEC);
      if (drm_fd_ < 0) {
        continue;
      }
      gbm_ = gbm_create_device(drm_fd_);
      if (gbm_) {
        Log("allocating on %s", node);
        return true;
      }
      ::close(drm_fd_);
      drm_fd_ = -1;
    }
    return false;
  }

  bool AllocRing() {
    for (uint32_t i = 0; i < kRingSlots; ++i) {
      Slot s;
      /*
       * Allocated to the modifier that was granted, not to a convenient one.
       * gbm_bo_map hands back a linear view of whatever layout this is, so the
       * fill below stays the same even when the buffer is tiled -- the driver
       * does the tiling on unmap, which costs the producer time but keeps the
       * shell measuring its own work rather than a format it cannot sample.
       */
      const uint64_t modifier = format_.modifier;
      /*
       * Allocate taller than the frame. A tiled layout is padded to whole tiles
       * plus whatever the driver adds on top, and the allocator here does not
       * apply that padding even when it reports a tiled modifier -- so the
       * buffer is exactly width*height*4, the importing driver computes the
       * padded size it actually needs, finds the dma-buf short, and refuses the
       * import. From out here that arrives as VK_ERROR_INVALID_EXTERNAL_HANDLE
       * with nothing naming a size, which is a long way from "the buffer is too
       * small". Measured: 1280x1440 needed 64 more rows, so round up to a whole
       * tile and add one more.
       *
       * PV_BENCH_PAD_ROWS overrides it, which is how the number above was
       * found: at 0 every frame failed to import, at 64 none did.
       */
      constexpr uint32_t kTileRows = 64;
      const char* pad_env = ::getenv("PV_BENCH_PAD_ROWS");
      const uint32_t alloc_height =
          pad_env != nullptr
              ? height_ + static_cast<uint32_t>(::atoi(pad_env))
              : ((height_ + kTileRows - 1) / kTileRows) * kTileRows + kTileRows;
#ifdef HAVE_GBM_BO_CREATE_WITH_MODIFIERS2
      // The usage matters to the allocator as much as the modifier does, and
      // which combination it will take differs per driver, so try the narrow
      // one before the broad one rather than guessing once and giving up.
      for (const uint32_t usage :
           {static_cast<uint32_t>(GBM_BO_USE_RENDERING),
            static_cast<uint32_t>(GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT),
            0u}) {
        s.bo = gbm_bo_create_with_modifiers2(
            gbm_, width_, alloc_height, format_.fourcc, &modifier, 1, usage);
        if (s.bo != nullptr) {
          if (i == 0) {
            Log("allocated with usage %#x", usage);
          }
          break;
        }
      }
#else
      // Pre-Mesa-21.1 gbm: modifiers without a usage argument. The allocator
      // picks the usage itself, so there is nothing to retry -- a failure here
      // means the modifier is refused outright.
      s.bo = gbm_bo_create_with_modifiers(gbm_, width_, alloc_height,
                                          format_.fourcc, &modifier, 1);
      if (s.bo != nullptr && i == 0) {
        Log("allocated (gbm without usage flags)");
      }
#endif
      if (!s.bo) {
        Log("gbm_bo_create_with_modifiers failed for slot %u (%ux%u fourcc %#x "
            "modifier %#llx)",
            i, width_, height_, format_.fourcc,
            static_cast<unsigned long long>(modifier));
        return false;
      }
      s.fd = gbm_bo_get_fd(s.bo);
      s.stride = gbm_bo_get_stride(s.bo);
      if (s.fd < 0) {
        Log("gbm_bo_get_fd failed for slot %u", i);
        return false;
      }
      uint32_t map_stride = 0;
      s.map = gbm_bo_map(s.bo, 0, 0, width_, height_, GBM_BO_TRANSFER_WRITE,
                         &map_stride, &s.map_data);  // map only the live rows
      if (!s.map) {
        Log("gbm_bo_map failed for slot %u", i);
        return false;
      }
      s.map_stride = map_stride ? map_stride : s.stride;
      s.modifier = gbm_bo_get_modifier(s.bo);
      s.planes = static_cast<uint32_t>(gbm_bo_get_plane_count(s.bo));
      slots_.push_back(s);
    }
    /*
     * The modifier the allocator actually chose, not the one asked for. Telling
     * the registry LINEAR for a buffer the driver laid out tiled is a lie it
     * cannot detect and the import fails with nothing to go on.
     */
    Log("ring of %u %ux%u buffers, stride %u modifier %#llx planes %u",
        kRingSlots, width_, height_, slots_[0].stride,
        static_cast<unsigned long long>(slots_[0].modifier), slots_[0].planes);
    if (slots_[0].modifier != format_.modifier) {
      Log("allocator gave modifier %#llx, not the granted %#llx; the import "
          "will fail",
          static_cast<unsigned long long>(slots_[0].modifier),
          static_cast<unsigned long long>(format_.modifier));
    }
    return true;
  }

  /*
   * A band that sweeps down the buffer: cheap to produce, and obvious on the
   * panel when it stops, which is the failure this benchmark can actually
   * suffer.
   */
  void Fill(const Slot& s, uint32_t frame) const {
    auto* base = static_cast<uint8_t*>(s.map);
    const uint32_t band = (frame * 4) % height_;
    for (uint32_t y = 0; y < height_; ++y) {
      auto* row = reinterpret_cast<uint32_t*>(base + static_cast<size_t>(y) *
                                                         s.map_stride);
      const uint32_t dist = (y >= band) ? (y - band) : (height_ - band + y);
      const uint32_t v = 255u - (dist * 255u / height_);
      const uint32_t color = 0xff000000u | (v << 16) | (0x40u << 8) | 0x80u;
      for (uint32_t x = 0; x < width_; ++x) {
        row[x] = color;
      }
    }
  }

  void Submit(Slot& s, uint32_t frame) {
    IhsFrame f{};
    f.struct_size = sizeof(f);
    f.format = format_;
    f.color_space = IHS_COLOR_SPACE_DEFAULT;
    f.color_range = IHS_COLOR_RANGE_DEFAULT;
    f.width = width_;
    f.height = height_;
    f.plane_count = 1;
    f.hdr = nullptr;
    f.buffer_id = frame % kRingSlots;
    /* The registry consumes the fds it is handed -- on the import and again as
     * redundant on a cache hit -- so it gets a dup and the ring keeps its own
     * for the life of the slot. */
    f.plane_fd[0] = ::dup(s.fd);
    f.plane_offset[0] = 0;
    f.plane_stride[0] = s.stride;
    if (f.plane_fd[0] < 0) {
      Log("dup failed for buffer %u", f.buffer_id);
      return;
    }

    int release_fence = -1;
    const int rc = ihs_pv_submit(view_, &f, -1, &release_fence);
    if (rc != IHS_PV_OK) {
      /*
       * The dup is gone either way: submit consumes plane_fd whatever it
       * returns, bar the malformed-frame rejection, and this frame is well
       * formed. Closing it here was a double close -- it ran 178 times in three
       * seconds on the import-failure path, each one able to take out an
       * unrelated fd that had reused the number.
       */
      if (submit_errors_++ % 60 == 0) {
        Log("submit failed: %d", rc);
      }
      return;
    }
    s.release_fence = release_fence;
    s.submitted = true;
    submitted_.fetch_add(1, std::memory_order_relaxed);
  }

  void Run() {
    uint32_t frame = 0;
    auto next = std::chrono::steady_clock::now();
    auto window = next;
    uint64_t window_frames = 0;
    uint64_t window_wait_ns = 0;

    while (running_.load(std::memory_order_acquire)) {
      next += kFramePeriod;

      if (suspended_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_until(next);
        continue;
      }

      Slot& s = slots_[frame % slots_.size()];

      /* Reclaim the slot before drawing over it. On the first pass round the
       * ring there is legitimately no fence yet. */
      const auto wait_began = std::chrono::steady_clock::now();
      if (s.submitted) {
        WaitAndCloseFence(s.release_fence);
        s.release_fence = -1;
      }
      window_wait_ns += static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - wait_began)
              .count());

      Fill(s, frame);
      Submit(s, frame);
      ++frame;
      ++window_frames;

      /*
       * Producer-side health, once a second. If this is not pinned at the
       * display rate the numbers downstream are describing a starved producer,
       * not the shell.
       */
      const auto now = std::chrono::steady_clock::now();
      if (now - window >= std::chrono::seconds(1)) {
        const double secs = std::chrono::duration<double>(now - window).count();
        Log("%.1f submits/s, release wait %.2f ms/frame, %llu total",
            static_cast<double>(window_frames) / secs,
            window_frames
                ? static_cast<double>(window_wait_ns) / window_frames / 1e6
                : 0.0,
            static_cast<unsigned long long>(
                submitted_.load(std::memory_order_relaxed)));
        window = now;
        window_frames = 0;
        window_wait_ns = 0;
      }

      /* Pace to the display rate. If a frame ran long, skip ahead rather than
       * chase the backlog at full speed. */
      if (next < now) {
        next = now;
      }
      std::this_thread::sleep_until(next);
    }
    Log("stopped after %llu submits",
        static_cast<unsigned long long>(
            submitted_.load(std::memory_order_relaxed)));
  }

  IhsPlatformView* const view_;
  const uint32_t width_;
  const uint32_t height_;
  IhsFormatModifier format_{};
  int drm_fd_ = -1;
  gbm_device* gbm_ = nullptr;
  std::vector<Slot> slots_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> suspended_{false};
  std::atomic<uint64_t> submitted_{0};
  uint64_t submit_errors_ = 0;
};

void OnResize(void* user_data, double w, double h) {
  static_cast<BenchView*>(user_data)->Resize(static_cast<uint32_t>(w),
                                             static_cast<uint32_t>(h));
}

void OnSuspended(void* user_data, uint8_t suspended) {
  static_cast<BenchView*>(user_data)->Suspend(suspended != 0);
}

/* The grant is gone and this producer does not chase it: a renegotiation
 * mid-run would change the path being measured without saying so. */
void OnRenegotiate(void* user_data) {
  Log("grant revoked; this run is over");
  static_cast<BenchView*>(user_data)->Suspend(true);
}

/* The destructor joins the submit thread, which is what the contract asks for:
 * the registry destroys the view once this returns, so a submit still in flight
 * would be running against freed memory. A flag would not do -- the thread can
 * already be inside ihs_pv_submit when it is cleared. */
void OnDispose(void* user_data) {
  delete static_cast<BenchView*>(user_data);
}

int Factory(const IhsPvCreateInfo* info,
            void* /*factory_user_data*/,
            IhsPlatformView* view,
            IhsPvCallbacks* out_callbacks,
            void** out_user_data) {
  auto* bench = new BenchView(view, static_cast<uint32_t>(info->width),
                              static_cast<uint32_t>(info->height));
  if (!bench->Start()) {
    delete bench;
    return IHS_PV_ERR_UNSUPPORTED;
  }
  out_callbacks->struct_size = sizeof(IhsPvCallbacks);
  out_callbacks->resize = OnResize;
  out_callbacks->set_suspended = OnSuspended;
  out_callbacks->renegotiate = OnRenegotiate;
  out_callbacks->dispose = OnDispose;
  *out_user_data = bench;
  return IHS_PV_OK;
}

}  // namespace

/* Platform-thread only, like every registry call. */
extern "C" __attribute__((visibility("default"))) void pv_bench_register(void) {
  const int rc = ihs_pv_register_factory("pv_bench", Factory, nullptr);
  Log("register_factory('pv_bench') rc=%d", rc);
}

extern "C" __attribute__((visibility("default"))) void pv_bench_unregister(
    void) {
  ihs_pv_unregister_factory("pv_bench");
}
