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
 * SHELL-ONLY host seam for the platform-view surface. This is NOT part of the
 * plugin ABI (docs/PLUGIN_ABI.md) — an out-of-tree plugin sees only
 * ihs/platform_view.h. It is the private contract between libihs_shared and the
 * one in-process shell that owns the backend, the compositor, and the
 * PlatformViewRegistry.
 *
 * libihs_shared is a thin forwarder: the plugin-facing ihs_pv_* entry points
 * carry no state of their own beyond the pure best-to-floor negotiate scoring;
 * everything else delegates to the host the shell installs. The shell's
 * PlatformViewRegistry is the single owner of the id->view lifecycle — an
 * ihs_pv plugin's factory becomes an ordinary registry factory, and its view an
 * ordinary registry-owned PlatformView, so there is no second registry and no
 * lifecycle vtable back into libihs_shared.
 *
 *   plugin  --ihs_pv_*-->  libihs_shared  --IhsPvHost-->  shell
 *
 * @view is the opaque handle the shell's factory handed the plugin;
 * libihs_shared passes it straight back through. The shell registers exactly
 * one host per process. Everything here is platform-thread only, matching
 * ihs/platform_view.h.
 */

#ifndef IHS_PLATFORM_VIEW_HOST_H_
#define IHS_PLATFORM_VIEW_HOST_H_

#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"
#include "ihs/platform_view.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Operations libihs_shared forwards to the shell. The shell implements each and
 * passes @user_data (its host context) back to every call. All are invoked on
 * the platform thread. A NULL entry means the capability is absent —
 * libihs_shared reports it to the plugin as unsupported / no-registry.
 *
 *   register_factory/unregister_factory
 *                 install the plugin's factory in the shell's
 *                 PlatformViewRegistry. When Flutter creates a view of that
 *                 type the registry (not libihs_shared) invokes the factory,
 *                 owns the returned view, and drives resize/touch/dispose into
 *                 the plugin's IhsPvCallbacks directly.
 *   query_capabilities/vulkan_context/egl_context
 *                 fill the out struct from the active Backend.
 *   grant         reserve the negotiated surface path for @view and write the
 *                 kind-specific payload back through the out-params (which the
 *                 shell caches on the view for the accessors below):
 *                   DRM_PLANE     -> *out_drm_plane_id
 *                   SOFTWARE_SHM  -> *out_shm_fd, *out_shm_stride
 *                   TEXTURE_*     -> no pull-side payload (the plugin submits)
 *   revoke        release whatever grant reserved for @view.
 *   grant_drm_plane_id/grant_shm_fd
 *                 read back the cached grant payload for @view.
 *   submit        hand a produced frame for @view to the compositor present
 *                 path.
 */
typedef struct IhsPvHost {
  size_t struct_size;
  void* user_data;

  int (*register_factory)(void* user_data,
                          const char* view_type,
                          IhsPvFactory factory,
                          void* factory_user_data);
  void (*unregister_factory)(void* user_data, const char* view_type);

  int (*query_capabilities)(void* user_data, IhsPvCapabilities* out);
  int (*vulkan_context)(void* user_data, IhsVulkanContext* out);
  int (*egl_context)(void* user_data, IhsEglContext* out);

  int (*grant)(void* user_data,
               IhsPlatformView* view,
               uint32_t kind,
               const IhsFormatModifier* format,
               uint32_t* out_drm_plane_id,
               int* out_shm_fd,
               size_t* out_shm_stride);
  void (*revoke)(void* user_data, IhsPlatformView* view);
  uint32_t (*grant_drm_plane_id)(void* user_data, IhsPlatformView* view);
  int (*grant_shm_fd)(void* user_data,
                      IhsPlatformView* view,
                      size_t* out_stride);

  int (*submit)(void* user_data,
                IhsPlatformView* view,
                const IhsFrame* frame,
                int acquire_fence_fd,
                int* out_release_fence_fd);
} IhsPvHost;

/*
 * Install (or replace) the shell's host. @host is borrowed for the process
 * lifetime (the shell keeps it alive) and may be NULL to detach (headless
 * teardown). Platform-thread only; call once at startup after the backend and
 * PlatformViewRegistry exist.
 */
IHS_EXPORT void ihs_pv_set_host(const IhsPvHost* host);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_PLATFORM_VIEW_HOST_H_ */
