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

#include <unistd.h>

#include <cstddef>

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

// Close a submitted frame's fds, honoring one rule: ihs_pv_submit consumes what
// it is handed. The host end of the path has always done this on its own error
// returns; these entry-point returns did not, so IHS_PV_ERR_NO_BACKEND meant
// "already closed" coming from the host and "still yours" coming from here --
// the same code, opposite ownership, with nothing for a plugin to test. A
// producer that guessed either way leaked fds or double-closed them, and a
// double close surfaces later on an unrelated fd that reused the number.
//
// One fd may back several planes, so close each distinct number once.
void close_frame_fds(const IhsFrame* frame, int acquire_fence_fd) {
  if (acquire_fence_fd >= 0) {
    ::close(acquire_fence_fd);
  }
  // The acquire fence is closed above regardless; the plane fds need
  // struct_size to actually reach them first. A caller that declared a frame
  // ending before plane_fd has no fds here to close, and reading them anyway
  // would close whatever follows its struct.
  if (frame == nullptr ||
      frame->struct_size <
          offsetof(IhsFrame, plane_offset) + sizeof(frame->plane_offset)) {
    return;
  }
  const uint32_t planes = frame->plane_count < 4u ? frame->plane_count : 4u;
  for (uint32_t i = 0; i < planes; ++i) {
    if (frame->plane_fd[i] < 0) {
      continue;
    }
    bool already_closed = false;
    for (uint32_t j = 0; j < i; ++j) {
      if (frame->plane_fd[j] == frame->plane_fd[i]) {
        already_closed = true;
        break;
      }
    }
    if (!already_closed) {
      ::close(frame->plane_fd[i]);
    }
  }
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

extern "C" const char* ihs_pv_assets_path(void) {
  const IhsPvHost* h = host();
  if (h == nullptr) {
    return nullptr;
  }
  // Appended to IhsPvHost after its initial layout. A shell built against the
  // older header reports a smaller struct_size and has no member here, so the
  // bounds check is what makes reading it safe rather than a guess.
  constexpr size_t kNeeded =
      offsetof(IhsPvHost, assets_path) + sizeof(h->assets_path);
  if (h->struct_size < kNeeded || h->assets_path == nullptr) {
    return nullptr;
  }
  return h->assets_path(h->user_data);
}

// The post_platform_task/is_platform_thread members were appended after
// assets_path; a host built against an older header has neither.
static const IhsPvHost* host_with_platform_thread() {
  const IhsPvHost* h = host();
  constexpr size_t kNeeded = offsetof(IhsPvHost, is_platform_thread) +
                             sizeof(IhsPvHost::is_platform_thread);
  return (h != nullptr && h->struct_size >= kNeeded) ? h : nullptr;
}

extern "C" int ihs_pv_post_platform_task(IhsPvTaskFn fn, void* user_data) {
  if (fn == nullptr) {
    return IHS_PV_ERR_INVALID;
  }
  const IhsPvHost* h = host_with_platform_thread();
  if (h == nullptr || h->post_platform_task == nullptr) {
    return IHS_PV_ERR_NO_REGISTRY;
  }
  return h->post_platform_task(h->user_data, fn, user_data);
}

extern "C" int ihs_pv_is_platform_thread(void) {
  const IhsPvHost* h = host_with_platform_thread();
  if (h == nullptr || h->is_platform_thread == nullptr) {
    return 0;
  }
  return h->is_platform_thread(h->user_data) != 0 ? 1 : 0;
}

extern "C" int ihs_pv_retire_buffer(IhsPlatformView* view, uint32_t buffer_id) {
  if (view == nullptr) {
    return IHS_PV_ERR_INVALID;
  }
  const IhsPvHost* h = host();
  if (h == nullptr) {
    return IHS_PV_ERR_NO_REGISTRY;
  }
  // Appended after is_platform_thread; a host built against an older header
  // has no such member.
  constexpr size_t kNeeded =
      offsetof(IhsPvHost, retire_buffer) + sizeof(IhsPvHost::retire_buffer);
  if (h->struct_size < kNeeded || h->retire_buffer == nullptr) {
    return IHS_PV_ERR_NO_BACKEND;
  }
  return h->retire_buffer(h->user_data, view, buffer_id);
}

// The fields of IhsLayer as first published; a caller built against that
// header passes exactly this much.
constexpr size_t kIhsLayerMinSize =
    offsetof(IhsLayer, reserved) + sizeof(IhsLayer::reserved);

// The layer's image (1.16), or null for one built without the field.
static const IhsImage* layer_image(const IhsLayer& layer) {
  return layer.struct_size >=
                 offsetof(IhsLayer, image) + sizeof(const IhsImage*)
             ? layer.image
             : nullptr;
}

extern "C" int ihs_pv_submit_layers(IhsPlatformView* view,
                                    const IhsLayer* layers,
                                    size_t layer_count,
                                    uint64_t seq,
                                    int* out_release_fence_fds) {
  // Malformed: nothing in it can be trusted to close from, so this closes
  // nothing and leaves every fd with the caller -- the one exception to the
  // ownership rule, the same one ihs_pv_submit makes.
  if (view == nullptr || (layers == nullptr && layer_count != 0) ||
      layer_count > IHS_PV_MAX_LAYERS) {
    return IHS_PV_ERR_INVALID;
  }
  for (size_t i = 0; i < layer_count; ++i) {
    if (layers[i].struct_size < kIhsLayerMinSize) {
      return IHS_PV_ERR_INVALID;
    }
    // A layer shows a frame or, from 1.16, an image: exactly one.
    const IhsImage* image = layer_image(layers[i]);
    if ((layers[i].frame == nullptr) == (image == nullptr)) {
      return IHS_PV_ERR_INVALID;
    }
    if (image != nullptr) {
      if (image->struct_size < sizeof(IhsImage) ||
          image->egl_image == nullptr) {
        return IHS_PV_ERR_INVALID;
      }
      continue;
    }
    // Every frame of a list carries its buffer_id: the shell keys each layer's
    // import on it, and never synthesises one here.
    if (layers[i].frame->struct_size <
        offsetof(IhsFrame, buffer_id) + sizeof(IhsFrame::buffer_id)) {
      return IHS_PV_ERR_INVALID;
    }
  }
  if (out_release_fence_fds != nullptr) {
    for (size_t i = 0; i < layer_count; ++i) {
      out_release_fence_fds[i] = -1;
    }
  }
  const auto close_all = [&] {
    for (size_t i = 0; i < layer_count; ++i) {
      close_frame_fds(layers[i].frame, layers[i].acquire_fence_fd);
    }
  };
  const IhsPvHost* h = host();
  if (h == nullptr) {
    close_all();
    return IHS_PV_ERR_NO_REGISTRY;
  }
  constexpr size_t kNeeded =
      offsetof(IhsPvHost, submit_layers) + sizeof(IhsPvHost::submit_layers);
  if (h->struct_size < kNeeded || h->submit_layers == nullptr) {
    close_all();
    return IHS_PV_ERR_NO_BACKEND;
  }
  return h->submit_layers(h->user_data, view, layers, layer_count, seq,
                          out_release_fence_fds);
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

  // EXPLICIT_REQUIRED is a demand, not a preference: a plugin asks for it
  // precisely so it is not handed a weaker grant it cannot service. Refuse
  // here, before grant(), so no plane id or shm fd is allocated for a
  // negotiation that cannot be honored.
  if (requirements->sync == IHS_PV_SYNC_EXPLICIT_REQUIRED &&
      caps.explicit_sync == 0) {
    return IHS_PV_ERR_UNSUPPORTED;
  }

  // Mutable: as of 1.15 grant() may replace the modifier with one it can
  // actually honor for this kind, and out->format below reports what came
  // back rather than what was asked for.
  IhsFormatModifier fmt = choose_format(requirements, &caps);
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
  // The one case where the fds stay the caller's: a frame this malformed has no
  // trustworthy plane_count or plane_fd to close from, and guessing would close
  // whatever integers happen to be there.
  if (frame == nullptr || frame->struct_size == 0) {
    return IHS_PV_ERR_INVALID;
  }
  const IhsPvHost* h = host();
  if (h == nullptr) {
    close_frame_fds(frame, acquire_fence_fd);
    return IHS_PV_ERR_NO_REGISTRY;
  }
  if (h->submit == nullptr) {
    close_frame_fds(frame, acquire_fence_fd);
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
      &ihs_pv_assets_path,        &ihs_pv_post_platform_task,
      &ihs_pv_is_platform_thread, &ihs_pv_retire_buffer,
      &ihs_pv_submit_layers,
  };
  return &api;
}

}  // namespace ihs::pv
