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

#include "shm_slots.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cerrno>
#include <map>
#include <mutex>
#include <string>

#include <drm/drm.h>
#include <drm/drm_fourcc.h>

#include "logging/logging.h"

namespace {

// The parts of gbm.h used here, declared rather than included: libgbm is
// loaded at run time, and its header need not be installed to build.
struct gbm_device;
struct gbm_bo;
union GbmBoHandle {
  void* ptr;
  int32_t s32;
  uint32_t u32;
  int64_t s64;
  uint64_t u64;
};
constexpr uint32_t kGbmBoUseRendering = 1u << 2;
constexpr uint32_t kGbmBoUseLinear = 1u << 4;

struct Gbm {
  gbm_device* (*create_device)(int fd){nullptr};
  gbm_bo* (*bo_create)(gbm_device* gbm,
                       uint32_t width,
                       uint32_t height,
                       uint32_t format,
                       uint32_t flags){nullptr};
  GbmBoHandle (*bo_get_handle)(gbm_bo* bo){nullptr};
  uint32_t (*bo_get_stride)(gbm_bo* bo){nullptr};
  void (*bo_destroy)(gbm_bo* bo){nullptr};

  [[nodiscard]] bool loaded() const { return bo_destroy != nullptr; }
};

// libgbm, loaded once and kept for the process. Empty when it is missing.
const Gbm& LoadGbm() {
  static Gbm gbm;
  static std::once_flag once;
  std::call_once(once, [] {
    void* const lib = ::dlopen("libgbm.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib == nullptr) {
      ihs::log::warn("[ihs_pv] shm slots: libgbm not loaded: {}", ::dlerror());
      return;
    }
    Gbm g;
    g.create_device = reinterpret_cast<decltype(g.create_device)>(
        ::dlsym(lib, "gbm_create_device"));
    g.bo_create =
        reinterpret_cast<decltype(g.bo_create)>(::dlsym(lib, "gbm_bo_create"));
    g.bo_get_handle = reinterpret_cast<decltype(g.bo_get_handle)>(
        ::dlsym(lib, "gbm_bo_get_handle"));
    g.bo_get_stride = reinterpret_cast<decltype(g.bo_get_stride)>(
        ::dlsym(lib, "gbm_bo_get_stride"));
    g.bo_destroy = reinterpret_cast<decltype(g.bo_destroy)>(
        ::dlsym(lib, "gbm_bo_destroy"));
    if (g.create_device == nullptr || g.bo_create == nullptr ||
        g.bo_get_handle == nullptr || g.bo_get_stride == nullptr ||
        g.bo_destroy == nullptr) {
      ihs::log::warn("[ihs_pv] shm slots: libgbm lacks a needed symbol");
      ::dlclose(lib);
      return;
    }
    gbm = g;
  });
  return gbm;
}

// The GBM device on a render node, opened on first use and kept for the
// process: a compositor has one or two GPUs, and a device per grant would
// cost an open per resize.
struct Device {
  int fd{-1};
  gbm_device* gbm{nullptr};
};

Device DeviceFor(const Gbm& gbm, uint64_t render_device) {
  static std::mutex mutex;
  static std::map<uint64_t, Device> devices;
  const std::lock_guard<std::mutex> lock(mutex);
  if (const auto it = devices.find(render_device); it != devices.end()) {
    return it->second;
  }
  const std::string path =
      "/dev/dri/renderD" + std::to_string(minor(render_device));
  Device d;
  d.fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (d.fd < 0) {
    ihs::log::warn("[ihs_pv] shm slots: open({}) failed (errno={})", path,
                   errno);
    return {};
  }
  struct stat st{};
  if (::fstat(d.fd, &st) != 0 || st.st_rdev != render_device) {
    ihs::log::warn("[ihs_pv] shm slots: {} is not render node {}:{}", path,
                   major(render_device), minor(render_device));
    ::close(d.fd);
    return {};
  }
  d.gbm = gbm.create_device(d.fd);
  if (d.gbm == nullptr) {
    ihs::log::warn("[ihs_pv] shm slots: no GBM device on {}", path);
    ::close(d.fd);
    return {};
  }
  devices.emplace(render_device, d);
  return d;
}

// A read-write dma-buf for @bo. gbm_bo_get_fd exports read-only, and the
// producer could not then map the buffer for writing.
int ExportReadWrite(const Gbm& gbm, int device_fd, gbm_bo* bo) {
  drm_prime_handle prime{};
  prime.handle = gbm.bo_get_handle(bo).u32;
  prime.flags = DRM_CLOEXEC | DRM_RDWR;
  prime.fd = -1;
  int rc;
  do {
    rc = ::ioctl(device_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
  } while (rc != 0 && (errno == EINTR || errno == EAGAIN));
  return rc == 0 ? prime.fd : -1;
}

}  // namespace

uint32_t ShmSlots::BytesPerPixel(const uint32_t fourcc) {
  switch (fourcc) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_XBGR8888:
      return 4;
    case DRM_FORMAT_RGB565:
      return 2;
    default:
      return 0;
  }
}

std::unique_ptr<ShmSlots> ShmSlots::Allocate(const uint64_t render_device,
                                             const uint32_t width,
                                             const uint32_t height,
                                             const uint32_t fourcc) {
  if (render_device == 0 || width == 0 || height == 0 ||
      BytesPerPixel(fourcc) == 0) {
    return nullptr;
  }
  const Gbm& gbm = LoadGbm();
  if (!gbm.loaded()) {
    return nullptr;
  }
  const Device device = DeviceFor(gbm, render_device);
  if (device.gbm == nullptr) {
    return nullptr;
  }
  std::unique_ptr<ShmSlots> slots(new ShmSlots());
  slots->width_ = width;
  slots->height_ = height;
  slots->fourcc_ = fourcc;
  for (uint32_t i = 0; i < kCount; ++i) {
    // The padding row; see the class comment.
    gbm_bo* bo = gbm.bo_create(device.gbm, width, height + 1, fourcc,
                               kGbmBoUseLinear | kGbmBoUseRendering);
    if (bo == nullptr) {
      // Some allocators refuse RENDERING for a format they only sample.
      bo =
          gbm.bo_create(device.gbm, width, height + 1, fourcc, kGbmBoUseLinear);
    }
    if (bo == nullptr) {
      ihs::log::warn(
          "[ihs_pv] shm slots: gbm_bo_create({}x{}, fourcc {:#x}) failed",
          width, height, fourcc);
      return nullptr;
    }
    const uint32_t stride = gbm.bo_get_stride(bo);
    const int fd = ExportReadWrite(gbm, device.fd, bo);
    // The dma-buf keeps the memory; the GEM handle is not needed past export.
    gbm.bo_destroy(bo);
    if (fd < 0) {
      ihs::log::warn("[ihs_pv] shm slots: read-write export failed (errno={})",
                     errno);
      return nullptr;
    }
    Slot& s = slots->slots_[i];
    s.fd = fd;
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
      ihs::log::warn("[ihs_pv] shm slots: fstat failed (errno={})", errno);
      return nullptr;
    }
    s.dev = st.st_dev;
    s.ino = st.st_ino;
    if (i == 0) {
      slots->stride_ = stride;
    } else if (stride != slots->stride_) {
      ihs::log::warn("[ihs_pv] shm slots: strides differ ({} and {})",
                     slots->stride_, stride);
      return nullptr;
    }
  }
  return slots;
}

ShmSlots::~ShmSlots() {
  for (const Slot& s : slots_) {
    if (s.fd >= 0) {
      ::close(s.fd);
    }
  }
}

bool ShmSlots::Matches(const uint32_t index, const int fd) const {
  if (index >= kCount || fd < 0) {
    return false;
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0) {
    return false;
  }
  return st.st_dev == slots_[index].dev && st.st_ino == slots_[index].ino;
}
