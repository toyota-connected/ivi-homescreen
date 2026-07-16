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

#include "backend/wayland_vulkan/wl_explicit_sync.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <algorithm>
#include <string>

#include <xf86drm.h>

// Dynamic dispatch, matching the backend (which owns the loader storage and
// calls VULKAN_HPP_DEFAULT_DISPATCHER.init()); do NOT define the storage here.
#define VULKAN_HPP_NO_EXCEPTIONS 1
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>

// clang-format off
// Order is load-bearing: the generated client header defines the traits and the
// interface tables (EMIT_INTERFACE_TABLES). clang-format must not reorder these.
#include "linux_drm_syncobj_client.hpp"  // linux_drm_syncobj_v1::client + traits
// clang-format on
#include <wl/proxy_impl.hpp>  // wl::construct
#include <wl/registry.hpp>    // wl::CRegistry
#include <wl/wl_ptr.hpp>      // wl::WlPtr

#include "logging/logging.h"

namespace wl_vulkan {

namespace {

const auto& d() {
  return vk::detail::defaultDispatchLoaderDynamic;
}

namespace sync = linux_drm_syncobj_v1::client;

// The three wp_linux_drm_syncobj objects carry no events — bare handlers.
class SyncobjManagerHandler
    : public sync::CWpLinuxDrmSyncobjManagerV1<SyncobjManagerHandler> {};
class SyncobjSurfaceHandler
    : public sync::CWpLinuxDrmSyncobjSurfaceV1<SyncobjSurfaceHandler> {};
class SyncobjTimelineHandler
    : public sync::CWpLinuxDrmSyncobjTimelineV1<SyncobjTimelineHandler> {};

constexpr uint32_t Hi(uint64_t v) {
  return static_cast<uint32_t>(v >> 32u);
}
constexpr uint32_t Lo(uint64_t v) {
  return static_cast<uint32_t>(v & 0xffffffffu);
}

// Resolve the /dev/dri node whose device number matches @p major/@p minor. A
// Wayland client has no DRM master, so the render node is opened purely for the
// drmSyncobj* ioctls. Returns "" if none matches.
std::string FindDrmNode(int64_t major_num, int64_t minor_num) {
  DIR* dir = opendir("/dev/dri");
  if (dir == nullptr) {
    return "";
  }
  std::string found;
  const dev_t want = makedev(static_cast<unsigned>(major_num),
                             static_cast<unsigned>(minor_num));
  for (dirent* e = readdir(dir); e != nullptr; e = readdir(dir)) {
    if (e->d_name[0] == '.') {
      continue;
    }
    std::string path = std::string("/dev/dri/") + e->d_name;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0 && S_ISCHR(st.st_mode) &&
        st.st_rdev == want) {
      found = path;
      break;
    }
  }
  closedir(dir);
  return found;
}

}  // namespace

struct ExplicitSync::Impl {
  VkDevice device = VK_NULL_HANDLE;
  int drm_fd = -1;
  uint32_t acquire_syncobj = 0;  // Vulkan signals this (acquire)
  uint32_t release_syncobj = 0;  // compositor signals this (release)
  VkSemaphore acquire_sem = VK_NULL_HANDLE;  // timeline, imported from acquire
  // Destruction order (reverse of declaration): surface, release_tl,
  // acquire_tl, manager — children before the manager that created them.
  wl::WlPtr<SyncobjManagerHandler> manager;
  wl::WlPtr<SyncobjTimelineHandler> acquire_tl;
  wl::WlPtr<SyncobjTimelineHandler> release_tl;
  wl::WlPtr<SyncobjSurfaceHandler> surface;
  uint64_t acquire_point = 0;
  uint64_t release_point = 0;
};

ExplicitSync::ExplicitSync() : impl_(std::make_unique<Impl>()) {}

ExplicitSync::~ExplicitSync() {
  if (impl_->acquire_sem != VK_NULL_HANDLE) {
    d().vkDestroySemaphore(impl_->device, impl_->acquire_sem, nullptr);
  }
  // Tear down the wp objects while the connection is still up (the backend
  // destroys ExplicitSync from its dtor, after App::~App() stopped Wayland
  // dispatch via Display::StopEvents() but before wl_display_disconnect). These
  // objects carry no events, so no OnXxx handler can race the destroy.
  impl_->surface.Reset();
  impl_->release_tl.Reset();
  impl_->acquire_tl.Reset();
  impl_->manager.Reset();
  if (impl_->drm_fd >= 0) {
    if (impl_->acquire_syncobj != 0) {
      drmSyncobjDestroy(impl_->drm_fd, impl_->acquire_syncobj);
    }
    if (impl_->release_syncobj != 0) {
      drmSyncobjDestroy(impl_->drm_fd, impl_->release_syncobj);
    }
    close(impl_->drm_fd);
  }
}

std::unique_ptr<ExplicitSync> ExplicitSync::Create(
    VkPhysicalDevice physical_device,
    VkDevice device,
    wl::CRegistry& registry,
    uint32_t manager_name,
    uint32_t manager_version,
    wl_display* display,
    wl_proxy* surface,
    std::string& err) {
  if (manager_name == 0) {
    err = "compositor does not advertise wp_linux_drm_syncobj_manager_v1";
    return nullptr;
  }
  if (surface == nullptr || display == nullptr) {
    err = "null surface/display";
    return nullptr;
  }

  std::unique_ptr<ExplicitSync> self(new ExplicitSync());
  Impl& s = *self->impl_;
  s.device = device;

  // Resolve + open the render node from the device's DRM properties.
  VkPhysicalDeviceDrmPropertiesEXT drm{};
  drm.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
  VkPhysicalDeviceProperties2 p2{};
  p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  p2.pNext = &drm;
  d().vkGetPhysicalDeviceProperties2(physical_device, &p2);
  if (drm.hasRender == VK_FALSE) {
    err = "device reports no DRM render node (VK_EXT_physical_device_drm)";
    return nullptr;
  }
  const std::string node = FindDrmNode(drm.renderMajor, drm.renderMinor);
  if (node.empty()) {
    err = "could not resolve /dev/dri render node";
    return nullptr;
  }
  s.drm_fd = open(node.c_str(), O_RDWR | O_CLOEXEC);
  if (s.drm_fd < 0) {
    err = "open(" + node + ") failed";
    return nullptr;
  }

  // Two DRM syncobjs: acquire (Vulkan signals) + release (compositor signals).
  if (drmSyncobjCreate(s.drm_fd, 0, &s.acquire_syncobj) != 0 ||
      drmSyncobjCreate(s.drm_fd, 0, &s.release_syncobj) != 0) {
    err = "drmSyncobjCreate failed";
    return nullptr;
  }

  // Bind the manager, then import both syncobjs as wp timeline objects. The fd
  // is dup'd by libwayland; flush the requests before closing the fds.
  wl_proxy* mgr_raw =
      registry.Bind<sync::wp_linux_drm_syncobj_manager_v1_traits>(
          manager_name,
          std::min(manager_version,
                   sync::wp_linux_drm_syncobj_manager_v1_traits::version));
  if (mgr_raw == nullptr) {
    err = "wl_registry_bind(wp_linux_drm_syncobj_manager_v1) failed";
    return nullptr;
  }
  s.manager.Attach(mgr_raw);

  int afd = -1;
  int rfd = -1;
  if (drmSyncobjHandleToFD(s.drm_fd, s.acquire_syncobj, &afd) != 0 ||
      drmSyncobjHandleToFD(s.drm_fd, s.release_syncobj, &rfd) != 0) {
    err = "drmSyncobjHandleToFD failed";
    if (afd >= 0) {
      close(afd);
    }
    return nullptr;
  }
  s.acquire_tl.Attach(
      wl::construct<
          sync::wp_linux_drm_syncobj_timeline_v1_traits,
          sync::wp_linux_drm_syncobj_manager_v1_traits::Op::ImportTimeline>(
          *s.manager.Get(), afd));
  s.release_tl.Attach(
      wl::construct<
          sync::wp_linux_drm_syncobj_timeline_v1_traits,
          sync::wp_linux_drm_syncobj_manager_v1_traits::Op::ImportTimeline>(
          *s.manager.Get(), rfd));
  wl_display_flush(display);
  close(afd);
  close(rfd);
  if (s.acquire_tl.Get()->IsNull() || s.release_tl.Get()->IsNull()) {
    err = "ImportTimeline produced no proxy";
    return nullptr;
  }

  // Import the acquire syncobj into Vulkan as a timeline semaphore (a fresh fd;
  // OPAQUE_FD, permanent import — Vulkan takes ownership of the fd on success).
  // The blit submit signals this timeline, which materializes the acquire point
  // the compositor waits on.
  VkSemaphoreTypeCreateInfo type{};
  type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  type.initialValue = 0;
  VkSemaphoreCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  sci.pNext = &type;
  if (d().vkCreateSemaphore(device, &sci, nullptr, &s.acquire_sem) !=
      VK_SUCCESS) {
    err = "vkCreateSemaphore (timeline) failed";
    return nullptr;
  }
  int vk_afd = -1;
  if (drmSyncobjHandleToFD(s.drm_fd, s.acquire_syncobj, &vk_afd) != 0) {
    err = "drmSyncobjHandleToFD (Vulkan import) failed";
    return nullptr;
  }
  VkImportSemaphoreFdInfoKHR ifi{};
  ifi.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
  ifi.semaphore = s.acquire_sem;
  ifi.flags = 0;  // permanent
  ifi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
  ifi.fd = vk_afd;
  if (d().vkImportSemaphoreFdKHR(device, &ifi) != VK_SUCCESS) {
    close(vk_afd);
    err = "vkImportSemaphoreFdKHR (OPAQUE_FD) unsupported";
    return nullptr;
  }

  // The per-surface syncobj object the acquire/release points are set on.
  s.surface.Attach(
      wl::construct<
          sync::wp_linux_drm_syncobj_surface_v1_traits,
          sync::wp_linux_drm_syncobj_manager_v1_traits::Op::GetSurface>(
          *s.manager.Get(), surface));
  if (s.surface.Get()->IsNull()) {
    err = "GetSurface produced no proxy";
    return nullptr;
  }

  ihs::log::info(
      "[WaylandVulkanBackend] explicit sync enabled (wp_linux_drm_syncobj on "
      "{})",
      node);
  return self;
}

VkSemaphore ExplicitSync::acquire_semaphore() const {
  return impl_->acquire_sem;
}

ExplicitSync::FramePoints ExplicitSync::NextFrame() {
  return {++impl_->acquire_point, ++impl_->release_point};
}

void ExplicitSync::SetSurfacePoints(const FramePoints& points) {
  impl_->surface.Get()->SetAcquirePoint(impl_->acquire_tl.Get()->GetProxy(),
                                        Hi(points.acquire), Lo(points.acquire));
  impl_->surface.Get()->SetReleasePoint(impl_->release_tl.Get()->GetProxy(),
                                        Hi(points.release), Lo(points.release));
}

bool ExplicitSync::SlotReleased(uint64_t release_point) const {
  if (release_point == 0) {
    return true;  // slot never committed
  }
  uint32_t handle = impl_->release_syncobj;
  uint64_t point = release_point;
  // Zero timeout: returns 0 iff the point is already signalled (compositor done
  // reading), a timeout error otherwise. The non-blocking release poll.
  const int r = drmSyncobjTimelineWait(
      impl_->drm_fd, &handle, &point, 1,
      /*timeout_nsec=*/0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, nullptr);
  return r == 0;
}

}  // namespace wl_vulkan
