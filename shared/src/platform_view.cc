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
 * Platform-view surface: a thin forwarder over the shell-installed IhsPvHost
 * (see ihs/platform_view_host.h). libihs_shared carries no view state — the
 * shell's PlatformViewRegistry owns the id->view lifecycle, so an ihs_pv
 * plugin's factory and views are ordinary registry entries. The only logic that
 * lives here is the pure best-to-floor negotiate scoring (choose_kind), so it
 * is unit-testable without a shell. With no host installed the surface is inert
 * but well-defined: every call reports "no registry / no backend /
 * unsupported".
 *
 * Threading: ihs/platform_view.h scopes the whole surface to the platform
 * thread. The mutex guards only the installed host pointer.
 */

#include "ihs/platform_view.h"
#include "ihs/platform_view_host.h"

#include "ihs_internal.hpp"

#include <cstring>
#include <mutex>

namespace {

std::mutex g_mutex;
const IhsPvHost* g_host = nullptr;

const IhsPvHost* host() {
  const std::lock_guard<std::mutex> lock(g_mutex);
  return g_host;
}

// Zero an out struct up to its caller-declared struct_size (preserving that
// field) so it is fully defined on every early return, before the host fills
// it. Clamped to this build's known size so a caller that over-declares
// struct_size cannot drive a write past the fields we understand. Every out
// struct in this ABI leads with `size_t struct_size`.
template <typename T>
void zero_out(T* out) {
  const size_t struct_size = out->struct_size;
  std::memset(out, 0, struct_size < sizeof(T) ? struct_size : sizeof(T));
  out->struct_size = struct_size;
}

// Surface paths in best-to-floor priority (ihs/platform_view.h): a zero-copy
// GPU import beats direct-scanout beats the universal software floor.
constexpr uint32_t kKindPriority[] = {
    IHS_PV_KIND_TEXTURE_DMABUF_IMPORT,
    IHS_PV_KIND_DRM_PLANE,
    IHS_PV_KIND_SOFTWARE_SHM,
};

// Pure policy: the highest-priority kind the plugin can produce AND the backend
// can grant, or IHS_PV_KIND_NONE when they do not intersect.
uint32_t choose_kind(uint32_t requested, uint32_t available) {
  for (const uint32_t kind : kKindPriority) {
    if ((requested & kind) != 0 && (available & kind) != 0) {
      return kind;
    }
  }
  return IHS_PV_KIND_NONE;
}

// Prefer a plugin format the backend also offers, else the plugin's first, else
// the backend's first, else "any" (zeroed).
IhsFormatModifier choose_format(const IhsPvRequirements* req,
                                const IhsPvCapabilities* caps) {
  IhsFormatModifier any{};
  if (req == nullptr || req->format_count == 0 || req->formats == nullptr) {
    if (caps != nullptr && caps->format_count > 0 && caps->formats != nullptr) {
      return caps->formats[0];
    }
    return any;
  }
  if (caps != nullptr && caps->format_count > 0 && caps->formats != nullptr) {
    for (size_t i = 0; i < req->format_count; ++i) {
      for (size_t j = 0; j < caps->format_count; ++j) {
        if (req->formats[i].fourcc == caps->formats[j].fourcc &&
            req->formats[i].modifier == caps->formats[j].modifier) {
          return req->formats[i];
        }
      }
    }
  }
  return req->formats[0];
}

}  // namespace

// --- plugin-facing surface (ihs/platform_view.h) ----------------------------

extern "C" int ihs_pv_register_factory(const char* view_type,
                                       IhsPvFactory factory,
                                       void* factory_user_data) {
  if (view_type == nullptr || factory == nullptr) {
    return IHS_PV_ERR_INVALID;
  }
  const IhsPvHost* h = host();
  if (h == nullptr || h->register_factory == nullptr) {
    return IHS_PV_ERR_NO_REGISTRY;
  }
  return h->register_factory(h->user_data, view_type, factory,
                             factory_user_data);
}

extern "C" void ihs_pv_unregister_factory(const char* view_type) {
  if (view_type == nullptr) {
    return;
  }
  const IhsPvHost* h = host();
  if (h != nullptr && h->unregister_factory != nullptr) {
    h->unregister_factory(h->user_data, view_type);
  }
}

extern "C" int ihs_pv_query_capabilities(IhsPvCapabilities* out) {
  if (out == nullptr || out->struct_size == 0) {
    return IHS_PV_ERR_INVALID;
  }
  zero_out(out);
  const IhsPvHost* h = host();
  if (h == nullptr || h->query_capabilities == nullptr) {
    return IHS_PV_ERR_NO_REGISTRY;
  }
  return h->query_capabilities(h->user_data, out);
}

extern "C" int ihs_pv_vulkan_context(IhsVulkanContext* out) {
  if (out == nullptr || out->struct_size == 0) {
    return IHS_PV_ERR_INVALID;
  }
  zero_out(out);
  const IhsPvHost* h = host();
  if (h == nullptr || h->vulkan_context == nullptr) {
    return IHS_PV_ERR_UNSUPPORTED;
  }
  return h->vulkan_context(h->user_data, out);
}

extern "C" int ihs_pv_egl_context(IhsEglContext* out) {
  if (out == nullptr || out->struct_size == 0) {
    return IHS_PV_ERR_INVALID;
  }
  zero_out(out);
  const IhsPvHost* h = host();
  if (h == nullptr || h->egl_context == nullptr) {
    return IHS_PV_ERR_UNSUPPORTED;
  }
  return h->egl_context(h->user_data, out);
}

extern "C" int ihs_pv_negotiate(IhsPlatformView* view,
                                const IhsPvRequirements* requirements,
                                IhsPvGrant* out) {
  if (requirements == nullptr || out == nullptr ||
      requirements->struct_size == 0 || out->struct_size == 0) {
    return IHS_PV_ERR_INVALID;
  }
  zero_out(out);
  const IhsPvHost* h = host();
  if (h == nullptr) {
    return IHS_PV_ERR_NO_REGISTRY;
  }
  if (h->query_capabilities == nullptr || h->grant == nullptr) {
    return IHS_PV_ERR_NO_BACKEND;
  }

  IhsPvCapabilities caps{};
  caps.struct_size = sizeof(caps);
  const int caps_rc = h->query_capabilities(h->user_data, &caps);
  if (caps_rc != IHS_PV_OK) {
    return caps_rc;
  }

  const uint32_t kind = choose_kind(requirements->kinds, caps.kinds);
  if (kind == IHS_PV_KIND_NONE) {
    return IHS_PV_ERR_UNSUPPORTED;
  }

  const IhsFormatModifier fmt = choose_format(requirements, &caps);
  uint32_t plane_id = 0;
  int shm_fd = -1;
  size_t shm_stride = 0;
  const int grant_rc =
      h->grant(h->user_data, view, kind, &fmt, &plane_id, &shm_fd, &shm_stride);
  if (grant_rc != IHS_PV_OK) {
    return grant_rc;
  }

  out->granted_kind = kind;
  // Honor at most what the plugin asked for, capped by backend explicit-sync.
  out->sync =
      (requirements->sync != IHS_PV_SYNC_IMPLICIT && caps.explicit_sync != 0)
          ? static_cast<uint32_t>(requirements->sync)
          : static_cast<uint32_t>(IHS_PV_SYNC_IMPLICIT);
  out->format = fmt;
  return IHS_PV_OK;
}

extern "C" uint32_t ihs_pv_grant_drm_plane_id(IhsPlatformView* view) {
  const IhsPvHost* h = host();
  if (h == nullptr || h->grant_drm_plane_id == nullptr) {
    return 0;
  }
  return h->grant_drm_plane_id(h->user_data, view);
}

extern "C" int ihs_pv_grant_shm_fd(IhsPlatformView* view, size_t* out_stride) {
  const IhsPvHost* h = host();
  if (h == nullptr || h->grant_shm_fd == nullptr) {
    return -1;
  }
  return h->grant_shm_fd(h->user_data, view, out_stride);
}

extern "C" int ihs_pv_submit(IhsPlatformView* view,
                             const IhsFrame* frame,
                             int acquire_fence_fd,
                             int* out_release_fence_fd) {
  if (out_release_fence_fd != nullptr) {
    *out_release_fence_fd = -1;
  }
  if (frame == nullptr || frame->struct_size == 0) {
    return IHS_PV_ERR_INVALID;
  }
  const IhsPvHost* h = host();
  if (h == nullptr) {
    return IHS_PV_ERR_NO_REGISTRY;
  }
  if (h->submit == nullptr) {
    return IHS_PV_ERR_NO_BACKEND;
  }
  return h->submit(h->user_data, view, frame, acquire_fence_fd,
                   out_release_fence_fd);
}

// --- host installation (ihs/platform_view_host.h) ---------------------------

extern "C" void ihs_pv_set_host(const IhsPvHost* host) {
  const std::lock_guard<std::mutex> lock(g_mutex);
  g_host = host;
}

// --- capability sub-table (IhsApi::platform_view) ---------------------------

namespace ihs::pv {

const IhsPlatformViewApi* platform_view_api() noexcept {
  static const IhsPlatformViewApi api = {
      sizeof(IhsPlatformViewApi), &ihs_pv_query_capabilities,
      &ihs_pv_vulkan_context,     &ihs_pv_egl_context,
      &ihs_pv_register_factory,   &ihs_pv_unregister_factory,
      &ihs_pv_negotiate,          &ihs_pv_grant_drm_plane_id,
      &ihs_pv_grant_shm_fd,       &ihs_pv_submit,
  };
  return &api;
}

}  // namespace ihs::pv
