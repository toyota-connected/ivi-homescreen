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
 * ihs_shared platform-view surface: how an out-of-tree FFI plugin publishes a
 * platform view and negotiates the surface path it renders through. It fronts
 * the shell-side PlatformViewRegistry (see
 * shell/platform/homescreen/platform_views/), which owns the id->instance
 * lifecycle. See docs/PLATFORM_VIEW_NEGOTIATION.md for the kind matrix and the
 * renegotiation contract, and docs/PLUGIN_ABI.md for the boundary rules.
 *
 * Lifecycle of one view:
 *   0. The plugin asks which surface kinds the active backend can grant
 *      (ihs_pv_query_capabilities) and picks its render strategy. The available
 *      set is backend-dependent and is offered from what the active backend
 *      exposes: SOFTWARE_SHM always, TEXTURE_DMABUF_IMPORT when there is a
 *      Vulkan or EGL context, DRM_PLANE only when that EGL context also has a
 *      GBM device (see the kind matrix in
 *      docs/PLATFORM_VIEW_NEGOTIATION.md). Only SOFTWARE_SHM is universal, so
 *      a plugin that handles the floor always has a path -- but one that needs
 *      dma-buf import must query for it rather than assume it.
 *   1. At load, the plugin installs a factory for its view type
 *      (ihs_pv_register_factory). Registration is process-global.
 *   2. When Flutter creates a view of that type, the registry invokes the
 *      factory on the platform thread. The factory allocates its per-view
 *      state, fills an IhsPvCallbacks table, and returns an opaque handle the
 *      registry stores. The registry owns view teardown: on dispose it invokes
 *      IhsPvCallbacks::dispose, then releases any negotiated grant.
 *   3. The plugin negotiates a surface path for the view (ihs_pv_negotiate),
 *      reads the kind-specific payload from the grant accessors, and renders.
 *   4. An output/mode change or plane-pressure event revokes the grant and
 *      fires IhsPvCallbacks::renegotiate; a plugin that ignores it is dropped
 *      to the software floor automatically.
 *
 * Threading: register/unregister-factory and negotiate are platform-thread
 * only. The factory and every IhsPvCallbacks entry are invoked on the platform
 * thread. Grant accessors are valid only between a successful negotiate and the
 * next renegotiate/dispose for that view. A plugin running on another thread
 * -- an FFI plugin is called on the Dart UI thread -- reaches the platform
 * thread with ihs_pv_post_platform_task.
 *
 * ihs_pv_submit is the exception: it may be called from any thread, which is
 * the point -- a producer drives it from whatever decode or render thread it
 * owns, and the registry locks per view to make that safe.
 *
 * What the registry cannot do is keep the view alive underneath such a call.
 * A view is unregistered, then IhsPvCallbacks::dispose runs, then the instance
 * is destroyed; no lock spans that, because the producer's thread is not the
 * registry's to block. So the plugin owns this guarantee: when its dispose
 * callback returns, no ihs_pv_submit for that view is in flight and none can
 * start. Joining the producing thread inside dispose is the usual way, and the
 * only reliable one -- a flag checked before the call still loses the race.
 *
 * Dispose is not only teardown. A create for an id that is already live
 * disposes the incumbent first, so a producer can be disposed while it is
 * running at full rate -- a hot restart, which re-creates views under the ids
 * the previous isolate used, is exactly that.
 */

#ifndef IHS_PLATFORM_VIEW_H_
#define IHS_PLATFORM_VIEW_H_

#include <stddef.h>
#include <stdint.h>

#include "ihs/ihs_export.h"

/*
 * An entry point a plugin may be built against but run without.
 *
 * A plugin links libihs_shared and is dlopen'd by the shell it finds at run
 * time, which is not always the one it was built against. A normal reference
 * to a symbol the running shell lacks fails the relocation, and the loader
 * rejects the whole plugin -- so an entry point documented as "NULL on an
 * older shell" could never actually report that, because nothing of the plugin
 * ran. Declaring the import weak makes the address NULL instead, which the
 * plugin can test.
 *
 * Consumers only: inside the library the definition stays strong.
 *
 * The test is on the symbol, not the return value, and both matter:
 *
 *     if (ihs_pv_assets_path) {
 *       const char* p = ihs_pv_assets_path();   // may still be NULL
 *     }
 *
 * Calling through the null address instead of testing it is a crash, so a
 * plugin that skips the guard is worse off than before.
 */
#if defined(__GNUC__) && !defined(ihs_shared_EXPORTS)
#define IHS_WEAK_IMPORT __attribute__((weak))
#else
#define IHS_WEAK_IMPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* An opaque per-view handle, valid from the factory call until the registry
 * disposes the view. Passed to negotiate and the grant accessors. */
typedef struct IhsPlatformView IhsPlatformView;

/* A DRM fourcc plus modifier, the format currency of the whole surface path. */
typedef struct IhsFormatModifier {
  uint32_t fourcc;   /* DRM_FORMAT_* */
  uint32_t reserved; /* padding; keep the u64 modifier 8-byte aligned */
  uint64_t modifier; /* DRM_FORMAT_MOD_* (DRM_FORMAT_MOD_LINEAR when unsure) */
} IhsFormatModifier;

/*
 * Colorimetry for a YUV frame. A camera or video decoder yields YUV whose
 * correct conversion to RGB depends on the color space and range; without them
 * the plane's CSC coefficients (or the compositor's sampler) guess and the
 * colors drift (the classic washed-out / oversaturated video). Ignored for RGB
 * frames. DEFAULT lets the backend apply its per-format default (BT.601 for SD,
 * BT.709 for HD, limited range).
 */
typedef enum IhsColorSpace {
  IHS_COLOR_SPACE_DEFAULT = 0,
  IHS_COLOR_SPACE_BT601 = 1,
  IHS_COLOR_SPACE_BT709 = 2,
  IHS_COLOR_SPACE_BT2020 = 3
} IhsColorSpace;

typedef enum IhsColorRange {
  IHS_COLOR_RANGE_DEFAULT = 0,
  IHS_COLOR_RANGE_FULL = 1,   /* 0..255 */
  IHS_COLOR_RANGE_LIMITED = 2 /* 16..235 (studio) */
} IhsColorRange;

/* Transfer function. PQ/HLG are HDR and pair with BT.2020 color space + the
 * mastering metadata below; the registry programs them onto the DRM plane's CSC
 * and the connector's HDR_OUTPUT_METADATA for direct-scanout HDR video. */
typedef enum IhsTransfer {
  IHS_TRANSFER_SDR = 0, /* sRGB / BT.709 gamma */
  IHS_TRANSFER_PQ = 1,  /* SMPTE ST 2084 */
  IHS_TRANSFER_HLG = 2  /* ARIB STD-B67 */
} IhsTransfer;

/*
 * HDR mastering metadata (SMPTE ST 2086 display primaries + CTA-861.3 light
 * levels), attached to a frame that is HDR. Maps to the kernel's
 * hdr_output_metadata / hdr_metadata_infoframe the registry sets on the
 * connector. Absent (NULL on the frame) means SDR. Zeroed optional fields mean
 * "unknown" — the registry sends what it has.
 */
typedef struct IhsHdrMetadata {
  size_t struct_size;
  uint8_t transfer;                /* IhsTransfer */
  uint8_t reserved[7];             /* pad; must be 0 */
  uint16_t display_primaries_x[3]; /* R,G,B, in 0.00002 units */
  uint16_t display_primaries_y[3];
  uint16_t white_point_x;
  uint16_t white_point_y;
  uint32_t max_display_mastering_luminance; /* cd/m^2 */
  uint32_t min_display_mastering_luminance; /* 0.0001 cd/m^2 */
  uint16_t max_content_light_level;         /* MaxCLL, cd/m^2 */
  uint16_t max_frame_average_light_level;   /* MaxFALL, cd/m^2 */
} IhsHdrMetadata;

/*
 * Surface paths a view can travel. A requirement carries a bitmask of the kinds
 * a plugin can produce; a grant names exactly one. Ordered best-to-floor:
 *   TEXTURE_DMABUF_IMPORT  plugin exports a dma-buf the engine compositor
 *                          samples through the texture registrar (zero copy). A
 *                          GL or Vulkan plugin renders with the shared context
 *                          from its native-context accessor and exports here;
 *                          there is no embedder-managed-texture kind — the
 *                          plugin owns its rendering.
 *   DRM_PLANE              bypass: the frame is scanned out directly on a KMS
 *                          overlay plane (zero GPU) — direct-scanout HDR video.
 *   SOFTWARE_SHM           universal floor: a CPU-filled shared-memory buffer.
 * A well-formed requirement that includes SOFTWARE_SHM never hard-fails.
 */
typedef enum IhsPvKind {
  IHS_PV_KIND_NONE = 0,
  IHS_PV_KIND_TEXTURE_DMABUF_IMPORT = 1u << 0,
  IHS_PV_KIND_DRM_PLANE = 1u << 1,
  IHS_PV_KIND_SOFTWARE_SHM = 1u << 2
} IhsPvKind;

/* Synchronization the plugin needs / the grant honors. Explicit sync uses an
 * in/out fence; availability differs across the device matrix, so a grant may
 * honor a weaker mode than requested unless EXPLICIT_REQUIRED was set. */
typedef enum IhsPvSync {
  IHS_PV_SYNC_IMPLICIT = 0,
  IHS_PV_SYNC_EXPLICIT_PREFERRED = 1,
  IHS_PV_SYNC_EXPLICIT_REQUIRED = 2
} IhsPvSync;

/* Where the view composites relative to the Flutter scene. */
typedef enum IhsPvZOrder {
  IHS_PV_Z_BELOW_FLUTTER = 0,
  IHS_PV_Z_ABOVE_FLUTTER = 1,
  IHS_PV_Z_INLINE = 2 /* interleaved with Flutter layers (hybrid composition) */
} IhsPvZOrder;

/* Result codes. Negotiation returns UNSUPPORTED when the requirement excludes
 * even the floor (SOFTWARE_SHM bit unset and no granted higher kind), and when
 * EXPLICIT_REQUIRED was asked for on a backend with no explicit sync.
 */
typedef enum IhsPvResult {
  IHS_PV_OK = 0,
  IHS_PV_ERR_INVALID = -1,     /* null/oversized arguments, bad struct_size */
  IHS_PV_ERR_NO_BACKEND = -2,  /* no active backend to negotiate against */
  IHS_PV_ERR_UNSUPPORTED = -3, /* requirement excludes even the floor */
  IHS_PV_ERR_NO_REGISTRY =
      -4 /* platform-view registry unavailable (headless) */
} IhsPvResult;

/*
 * What the active backend can grant right now, independent of any view — the
 * first thing a plugin asks.
 *
 * @backend_key is the active backend's registry key ("wayland-egl",
 * "drm-kms-vulkan", "wayland-leased-drm-vulkan", ...), registry-owned and
 * stable for the process. It is informational: a plugin selects its native
 * context by calling the accessor it wants — each returns
 * IHS_PV_ERR_UNSUPPORTED when the active backend does not provide it — not by
 * switching on the key. There is no fixed backend-type enum; the shell's
 * BackendDescriptor registry is string-keyed and new backends (the leased-DRM
 * tiers) add keys without an ABI change. A leased-DRM tier is a DRM client that
 * obtained its fd from a compositor lease rather than opening a card: it
 * exposes the DRM/Vulkan context and DRM_PLANE, not a Wayland surface.
 *
 * @kinds is a bitmask of the IhsPvKind values this backend can produce; it
 * varies by backend and can change on an output/mode switch — or a lease
 * revocation — the same event that revokes a grant. @explicit_sync is 1 when
 * explicit fences are available. @formats/@format_count is the fourcc+modifier
 * set the dma-buf/shm kinds can offer, pointing into registry-owned storage
 * valid until the next query on the calling thread.
 */
typedef struct IhsPvCapabilities {
  size_t struct_size;
  const char* backend_key; /* active BackendDescriptor key; registry-owned */
  uint32_t kinds;        /* bitmask of IhsPvKind the active backend can grant */
  uint8_t explicit_sync; /* 1 if EXPLICIT sync is available */
  uint8_t reserved[3];   /* pad; must be 0 */
  const IhsFormatModifier* formats;
  size_t format_count;
} IhsPvCapabilities;

/*
 * Report the surface kinds (and formats) the active backend can grant into
 * @out. Returns IHS_PV_OK, IHS_PV_ERR_NO_BACKEND when no backend is active, or
 * IHS_PV_ERR_NO_REGISTRY in a headless build. Platform-thread only. The result
 * is a snapshot: it can change across an output/mode switch, so a long-lived
 * plugin re-queries from its renegotiate callback.
 */
IHS_EXPORT int ihs_pv_query_capabilities(IhsPvCapabilities* out);

/*
 * Absolute path to the running bundle's flutter_assets directory, or NULL if
 * the shell has no host installed or has not resolved a bundle. Borrowed:
 * owned by the shell and valid for the engine's lifetime; copy it if you need
 * to outlive the call.
 *
 * A plugin that resolves its own assets by relative path needs this. The
 * compositor-surface path receives the directory as an initialize() argument;
 * the platform-view path had no equivalent, so such a plugin had nothing to
 * resolve against.
 *
 * Added after 1.0, so a shell can predate it. IHS_WEAK_IMPORT (see above)
 * makes that case a null symbol rather than a failed load; test the symbol
 * before calling it:
 *
 *     const char* p = ihs_pv_assets_path ? ihs_pv_assets_path() : NULL;
 */
IHS_EXPORT IHS_WEAK_IMPORT const char* ihs_pv_assets_path(void);

/*
 * A task for the platform thread; see ihs_pv_post_platform_task.
 */
typedef void (*IhsPvTaskFn)(void* user_data);

/*
 * Run @fn(@user_data) on the platform thread.
 *
 * Most of this surface -- register/unregister-factory, negotiate, the
 * capability query and the native-context accessors -- is platform-thread
 * only, but a plugin loaded over Dart FFI is called on the Dart UI thread and
 * has no platform thread of its own to be on. This is how it gets there: post
 * the work and let it run in order with the registry's own callbacks.
 *
 * Returns IHS_PV_OK when the task was queued, IHS_PV_ERR_INVALID for a NULL
 * @fn, IHS_PV_ERR_NO_REGISTRY with no host installed, and
 * IHS_PV_ERR_NO_BACKEND when the engine is not running (before start-up, or
 * during shutdown). The task runs exactly once when this returns IHS_PV_OK,
 * with one exception: a task still queued when the engine shuts down is
 * dropped without running, so @user_data must not own anything that only
 * @fn can release.
 *
 * Any thread may call this, including the platform thread itself; the task
 * then runs after the current one returns, never inline. So never block the
 * platform thread waiting for a task it posted -- check
 * ihs_pv_is_platform_thread() and call directly instead.
 *
 * Added after 1.0; IHS_WEAK_IMPORT, so test the symbol before calling it.
 */
IHS_EXPORT IHS_WEAK_IMPORT int ihs_pv_post_platform_task(IhsPvTaskFn fn,
                                                         void* user_data);

/*
 * 1 when the calling thread is the platform thread, else 0 (including with no
 * host or no running engine). Lets a plugin that needs a platform-thread
 * result synchronously run inline instead of posting and waiting on itself.
 *
 * Added after 1.0; IHS_WEAK_IMPORT, so test the symbol before calling it.
 */
IHS_EXPORT IHS_WEAK_IMPORT int ihs_pv_is_platform_thread(void);

/*
 * The backend's Vulkan objects, offered to a plugin that renders on a Vulkan
 * backend. Handles are void* so this header stays free of vulkan.h; cast to the
 * matching Vk* type.
 *
 * Reusing this device is preferred but NOT required. A plugin that reuses it
 * avoids a second VkDevice, shares the backend's memory budget/allocator, and
 * gets guaranteed dma-buf modifier compatibility. But a plugin built on an
 * engine that insists on its own device (Filament, some maplibre builds) MAY
 * create its own, render there, and export a dma-buf that ihs_pv_submit hands
 * to the compositor for cross-device import — still zero-copy at scanout,
 * subject to the format+modifier negotiation confirming the compositor can
 * import it. In that case the queue-sharing note below does not apply (the
 * plugin owns its own queue); it applies only to a plugin that reuses @queue.
 *
 * The plugin MUST resolve every Vulkan function through
 * @get_instance_proc_addr. It is the same interposed loader the engine is
 * given: it returns the real entry points, except that
 * vkQueueSubmit/vkQueueSubmit2/vkQueuePresentKHR (and their device-level
 * resolution) are wrapped to take the shared-queue lock. So a plugin that
 * resolves through it is serialized on the queue automatically, the same way
 * the engine is — resolving through any other loader reintroduces the data
 * race.
 *
 * A plugin cannot add extensions or features to an already-created device, so
 * @device_extensions lists what was enabled: dma-buf export needs
 * VK_EXT_external_memory_dma_buf + VK_KHR_external_memory_fd +
 * VK_EXT_image_drm_format_modifier. Absent an extension, the plugin falls back
 * to the floor.
 *
 * Do not read explicit-sync availability out of this list. What the registry
 * honors is reported by IhsPvCapabilities::explicit_sync and, per view, by the
 * granted IhsPvGrant::sync -- gate on those. VK_KHR_synchronization2 in
 * particular is enabled only where the device has it, since nothing in the
 * shell or the engine issues a synchronization2 command, so its presence here
 * says what the device can do and not what the surface guarantees.
 *
 * Queue sharing: the Flutter embedder Vulkan API takes a single VkQueue and
 * exposes no engine->backend hand-off semaphore, so the engine, the backend and
 * the plugin all submit on @queue (one graphics queue) — host access to a
 * VkQueue must be externally synchronized (issue #208). Per-submit
 * serialization is handled for you by the interposed loader above; use
 * @queue_lock / @queue_unlock only to make a multi-call sequence atomic (e.g. a
 * vkQueueSubmit immediately followed by a vkQueuePresentKHR). Both are NULL
 * when the active backend is not Vulkan.
 */
typedef struct IhsVulkanContext {
  size_t struct_size;
  void* instance;        /* VkInstance */
  void* physical_device; /* VkPhysicalDevice */
  void* device;          /* VkDevice */
  void* queue;           /* VkQueue (shared engine/backend/plugin) */
  uint32_t queue_family_index;
  uint32_t api_version;         /* VK_API_VERSION the device was created with */
  void* get_instance_proc_addr; /* interposed PFN_vkGetInstanceProcAddr */
  const char* const* device_extensions;
  size_t device_extension_count;
  const char* const* instance_extensions;
  size_t instance_extension_count;
  void (*queue_lock)(void); /* only for multi-call atomicity; see above */
  void (*queue_unlock)(void);
  /* The backend's VmaAllocator, so a plugin allocates from the same memory
     pools / budget / defrag rather than a competing allocator. NULL until VMA
     is integrated in the backend (allocate manually or bring your own until
     then). Sharing it across the FFI boundary requires the plugin to link an
     ABI-compatible VMA build — VmaAllocator is opaque internal state, so a
     mismatched VMA version is undefined; an out-of-tree plugin that cannot
     guarantee this should allocate manually. */
  void* vma_allocator; /* VmaAllocator */
} IhsVulkanContext;

/*
 * Fill @out with the active backend's Vulkan objects. Returns IHS_PV_OK, or
 * IHS_PV_ERR_UNSUPPORTED when the active backend is not Vulkan (query
 * IhsPvCapabilities first / gate on the granted kind). Platform-thread only;
 * the handles are valid for the backend's lifetime.
 */
IHS_EXPORT int ihs_pv_vulkan_context(IhsVulkanContext* out);

/*
 * The backend's EGL/GBM objects, for a GL plugin that renders into an FBO and
 * exports a dma-buf (on an EGL backend — wayland-egl or drm-kms-egl). Handles
 * are void* so this header stays free of EGL/gbm headers; cast to the matching
 * EGL or gbm type. @egl_context is the shared context the plugin makes current;
 * @gbm_device is set on a DRM backend (allocate scanout-capable buffers there)
 * and NULL on wayland-egl (export via EGL_MESA_image_dma_buf_export instead).
 * All NULL on a Vulkan backend — use ihs_pv_vulkan_context there. A plugin that
 * already holds dma-bufs (a libcamera/v4l2 camera, a video decoder) needs no
 * context at all: it just calls ihs_pv_submit.
 */
typedef struct IhsEglContext {
  size_t struct_size;
  void* egl_display; /* EGLDisplay */
  void* egl_context; /* EGLContext (shared) */
  void* egl_config;  /* EGLConfig */
  void* gbm_device;  /* struct gbm_device* on a DRM backend, else NULL */
} IhsEglContext;

/*
 * Fill @out with the active backend's EGL/GBM objects. Returns IHS_PV_OK, or
 * IHS_PV_ERR_UNSUPPORTED when the active backend is not an EGL backend.
 * Platform-thread only; the handles are valid for the backend's lifetime.
 */
IHS_EXPORT int ihs_pv_egl_context(IhsEglContext* out);

/*
 * What a plugin can produce and needs. struct_size-first; additive growth only.
 * @kinds is a bitmask of IhsPvKind. @formats/@format_count is the acceptable
 * fourcc+modifier set for the dma-buf/shm kinds (ignored for the
 * bypass kinds that own their own surface); an empty list means "any the
 * backend offers". @needs_alpha, @sync (IhsPvSync), @z_order (IhsPvZOrder).
 */
typedef struct IhsPvRequirements {
  size_t struct_size;
  uint32_t kinds;
  const IhsFormatModifier* formats;
  size_t format_count;
  uint8_t needs_alpha; /* 0 or 1 */
  uint8_t sync;        /* IhsPvSync */
  uint8_t z_order;     /* IhsPvZOrder */
  uint8_t reserved;    /* pad to 4-byte boundary; must be 0 */
} IhsPvRequirements;

/*
 * The negotiated path. struct_size-first. @granted_kind is exactly one
 * IhsPvKind; @format is the chosen fourcc+modifier (meaningful for the
 * dma-buf/shm kinds); @sync is the IhsPvSync actually honored. The
 * kind-specific payload (wl_surface*, plane id, import target, shm fd) is NOT
 * in this struct — read it with the ihs_pv_grant_* accessors keyed on
 * @granted_kind. The grant is valid until the next renegotiate/dispose.
 */
typedef struct IhsPvGrant {
  size_t struct_size;
  uint32_t granted_kind; /* one IhsPvKind */
  uint32_t sync;         /* IhsPvSync honored */
  IhsFormatModifier format;
} IhsPvGrant;

/* clang-format off */
/*
 * Per-view event callbacks the factory fills. The registry drives these on the
 * platform thread, in response to the flutter/platform_views channel and to
 * backend changes. All optional (leave NULL). @user_data is the handle the
 * factory returned.
 *
 *   resize                           the view's new size in PHYSICAL device
 *                                    pixels (already DPR-scaled) — a GPU
 *                                    renderer should render at this resolution
 *                                    for crisp output. Position and z-order come
 *                                    from the Flutter layer, not a callback.
 *   on_touch                         a pointer/touch sequence hit the view;
 *                                    @pointer_data holds @point_count contacts
 *                                    in @pointer_data_size doubles *in total*,
 *                                    so one contact's stride is
 *                                    @pointer_data_size / @point_count -- the
 *                                    size is the whole buffer, not the stride.
 *                                    Within a contact, x sits at offset 7 and y
 *                                    at offset 8 (raw Flutter PointerCoords
 *                                    order), in view-local logical pixels (a
 *                                    plugin remaps to its own space, e.g. a
 *                                    CarPlay dongle's).
 *   accept_gesture/reject_gesture    gesture-arena arbitration for a sequence
 *                                    already delivered to the view.
 *   set_suspended                    the view left (1) or re-entered (0) the
 *                                    composited scene — scrolled off, fully
 *                                    occluded, or on a display that has been
 *                                    unplugged. NOT teardown; the view stays
 *                                    alive and its grant stays held. A producing
 *                                    plugin should pause its decode/render while
 *                                    suspended to save power (a camera, a video,
 *                                    a live tile), and resume on 0. Edge
 *                                    triggered: a suspended view is not told
 *                                    again, so 1 and 0 always alternate.
 *   renegotiate                      a grant was revoked (output/mode/plane
 *                                    change, or a lease withdrawn); re-call
 *                                    ihs_pv_negotiate or fall back to the floor.
 *   dispose                          the view is closing — the last call; the
 *                                    registry frees the instance and releases
 *                                    the grant after it returns.
 */
/* clang-format on */
typedef struct IhsPvCallbacks {
  size_t struct_size;
  void (*resize)(void* user_data, double width, double height);
  /* pointer_data_size is the total number of doubles in pointer_data across
   * all contacts, not a per-contact stride; divide by point_count for that. */
  void (*on_touch)(void* user_data,
                   int32_t action,
                   int32_t point_count,
                   size_t pointer_data_size,
                   const double* pointer_data);
  void (*accept_gesture)(void* user_data);
  void (*reject_gesture)(void* user_data);
  void (*set_suspended)(void* user_data, uint8_t suspended);
  void (*renegotiate)(void* user_data);
  void (*dispose)(void* user_data);
} IhsPvCallbacks;

/*
 * The create request handed to a factory. struct_size-first. The factory
 * allocates per-view state, fills @out_callbacks, stores its handle in
 * @out_user_data, and returns IHS_PV_OK; any other code aborts the create.
 * @view remains valid for negotiate/accessors until dispose.
 */
typedef struct IhsPvCreateInfo {
  size_t struct_size;
  int32_t id;
  const char* view_type;
  int32_t direction;
  double left;
  double top;
  double width;
  double height;
  const uint8_t* params; /* creationParams payload, may be NULL */
  size_t params_size;
} IhsPvCreateInfo;

typedef int (*IhsPvFactory)(const IhsPvCreateInfo* info,
                            void* factory_user_data,
                            IhsPlatformView* view,
                            IhsPvCallbacks* out_callbacks,
                            void** out_user_data);

/*
 * Install (or replace) the factory for @view_type. @factory_user_data is
 * passed back to every factory call. Returns IHS_PV_OK, or IHS_PV_ERR_INVALID
 * for a null type/factory. Platform-thread only.
 */
IHS_EXPORT int ihs_pv_register_factory(const char* view_type,
                                       IhsPvFactory factory,
                                       void* factory_user_data);

/* Remove the factory for @view_type. Live views of that type keep running. */
IHS_EXPORT void ihs_pv_unregister_factory(const char* view_type);

/*
 * Score @requirements against the active backend and live capability probes
 * (plane availability, format+modifier intersection) and write the best grant
 * to @out. Returns IHS_PV_OK on a grant (including the software floor), or a
 * negative IhsPvResult. The grant's kind-specific payload is then read via the
 * accessors below. Re-callable to renegotiate after a revocation.
 */
IHS_EXPORT int ihs_pv_negotiate(IhsPlatformView* view,
                                const IhsPvRequirements* requirements,
                                IhsPvGrant* out);

/* clang-format off */
/*
 * Kind-specific grant payload, valid only for the current grant on @view.
 * Each returns 0 / NULL when the current granted_kind does not match.
 *   drm_plane_id   DRM_PLANE: the reserved KMS plane object id.
 *   shm_fd         SOFTWARE_SHM: the shared-memory fd plus stride the plugin
 *                  fills in grant.format; ownership stays with the registry.
 *                  There is no software native-context accessor — the software
 *                  backend is a CPU blitter over a pluggable sink (DRM dumb,
 *                  fbdev, file, SPI LCD), so the plugin never touches the sink;
 *                  it writes grant.format (which the active sink dictates:
 *                  RGB565 for an ST7789, XRGB8888 for fbdev, ...) and the sink
 *                  consumes the buffer.
 * TEXTURE_DMABUF_IMPORT has no pull-side payload: the plugin renders with the
 * shared context from its native-context accessor and exports a dma-buf per
 * frame via ihs_pv_submit (declared with the producer surface).
 */
/* clang-format on */
IHS_EXPORT uint32_t ihs_pv_grant_drm_plane_id(IhsPlatformView* view);
IHS_EXPORT int ihs_pv_grant_shm_fd(IhsPlatformView* view, size_t* out_stride);

/*
 * A produced frame: a dma-buf with up to 4 planes (NV12/YUYV and other
 * multi-planar YUV formats — a camera or video decoder — need more than one).
 * @format is the fourcc+modifier matching the grant; @plane_fd/@plane_offset/
 * @plane_stride describe each plane (one fd may back several planes). Fill
 * @plane_count planes.
 *
 * The allocation must be at least the size the granted format+modifier implies,
 * which is NOT width*height*bpp once a tiled modifier is in play: a tiled
 * layout is padded to whole tiles, and an importing driver can want more still.
 * A plugin cannot compute that padding, and an allocator may report a tiled
 * modifier for a buffer it sized as if linear -- so allocate with headroom
 * (rounding the height up to a tile and adding one more is enough in practice)
 * rather than to the exact frame geometry. An import refused for this reason
 * fails with no indication of the size it wanted; the registry logs what the
 * image needed against what the dma-buf held.
 *
 * @buffer_id names WHICH buffer of the plugin's ring this frame is (0..N-1,
 * stable for that buffer's lifetime). The registry imports each distinct
 * buffer_id once — creating the VkImage / KMS framebuffer that aliases the
 * dma-buf — and reuses that import on every later submit of the same id, so a
 * steady stream of frames costs no per-frame allocation. A plugin that hands a
 * fresh dma-buf every frame (no ring) uses a rolling id; one that cycles a
 * fixed ring reuses its ids.
 *
 * FD OWNERSHIP: ihs_pv_submit consumes @plane_fd. The registry closes them --
 * on the import, as redundant handles on a cache hit, and on every path that
 * refuses the frame. That holds whatever ihs_pv_submit returns, with one
 * exception: a frame so malformed it is rejected on struct_size (or a NULL
 * frame) leaves them alone, because nothing in it can be trusted to close from.
 * So a plugin closes its own fds only when it never handed them over.
 *
 * A plugin that keeps a ring passes dups and holds its originals for the life
 * of the slot. One that closes after a failed submit double-closes, and a
 * double close does not report itself -- it lands several submits later on an
 * unrelated fd that reused the number.
 */
typedef struct IhsFrame {
  size_t struct_size;
  IhsFormatModifier format;
  uint8_t color_space; /* IhsColorSpace — YUV frames; DEFAULT for RGB */
  uint8_t color_range; /* IhsColorRange */
  uint8_t reserved[2]; /* pad; must be 0 */
  uint32_t width;      /* source pixels; the registry scales to the view rect */
  uint32_t height;
  uint32_t plane_count; /* 1..4 */
  int plane_fd[4];      /* dma-buf fd per plane */
  uint32_t plane_offset[4];
  uint32_t plane_stride[4];
  const IhsHdrMetadata* hdr; /* NULL = SDR; set for PQ/HLG HDR video */
  uint32_t buffer_id;        /* ring-buffer identity for import caching */
} IhsFrame;

/*
 * Hand the registry a produced @frame for @view. Used by all kinds
 * (TEXTURE_DMABUF_IMPORT, SOFTWARE_SHM, and the registry-driven DRM_PLANE).
 * Callable from any thread, unlike the rest of this surface; see the threading
 * note at the top of this header for the one thing that requires -- that no
 * call is in flight once IhsPvCallbacks::dispose has returned.
 * @acquire_fence_fd signals when the plugin's production completed (-1 for
 * implicit sync); the registry takes ownership of the fd, on the same terms as
 * @plane_fd above -- consumed whatever this returns, except on the malformed-
 * frame rejection, which closes nothing.
 * *out_release_fence_fd, when the registry sets it to a non-negative fd, is a
 * release fence that fires when the buffer is free to reuse: the plugin owns
 * that fd and must close it (waiting on it first before overwriting the
 * buffer). The registry may return one under any sync mode — the DRM_PLANE path
 * returns a per-buffer release eventfd even on an implicit-sync grant, since it
 * has no other back-channel to the producer. -1 means no fence for this submit
 * and release rides IhsPvCallbacks / the native protocol; a plugin must
 * therefore always close a returned fd regardless of the negotiated sync mode.
 * The plugin keeps whatever buffer ring it wants (double, triple, N); the
 * registry never counts them. Returns IHS_PV_OK or a negative IhsPvResult.
 */
IHS_EXPORT int ihs_pv_submit(IhsPlatformView* view,
                             const IhsFrame* frame,
                             int acquire_fence_fd,
                             int* out_release_fence_fd);

/*
 * Tell the registry that @buffer_id will not be submitted again for @view, so
 * it can drop what it cached for that buffer: the VkImage or GL texture the
 * dma-buf was imported into, and the duplicate fds kept for direct scanout.
 *
 * The import cache is keyed by buffer_id and never forgets an id on its own,
 * which is right for a fixed ring but not for a producer whose buffers come
 * and go -- a Wayland server passing through its clients' buffers, or a
 * decoder that reallocates on a rendition switch. Without this, such a
 * producer either grows the cache by one import per buffer for the life of
 * the view, or reuses ids and has a new dma-buf sampled through the stale
 * import of an old one.
 *
 * Nothing is freed while a present can still use it. A buffer that is the
 * view's current frame stays on screen until a later submit supersedes it and
 * is dropped then; others are dropped after the in-flight frames that may
 * still bind them have retired. Retiring an id the view never saw, or one
 * already retired, is a no-op.
 *
 * Do not submit a retired id again. On the drm-kms backends the framebuffer
 * a buffer was scanned out through is cached separately, keyed by the same
 * id, and is not yet dropped by this call (that cache is bounded on its own),
 * so a re-used id could reach a plane through the stale framebuffer.
 *
 * Callable from any thread, like ihs_pv_submit and under the same dispose
 * rule: no call may be in flight once IhsPvCallbacks::dispose has returned.
 * Returns IHS_PV_OK, IHS_PV_ERR_INVALID for a NULL @view,
 * IHS_PV_ERR_NO_REGISTRY with no host, or IHS_PV_ERR_NO_BACKEND when the host
 * predates this call.
 *
 * Added after 1.0; IHS_WEAK_IMPORT, so test the symbol before calling it.
 */
IHS_EXPORT IHS_WEAK_IMPORT int ihs_pv_retire_buffer(IhsPlatformView* view,
                                                    uint32_t buffer_id);

/*
 * How a layer's buffer is oriented. The values and meaning are
 * wl_output.transform's: the rotation (counter-clockwise) and reflection the
 * producer already applied to the buffer, which the compositor undoes when it
 * draws. FLIPPED mirrors around the vertical axis before rotating.
 */
typedef enum IhsTransform {
  IHS_TRANSFORM_NORMAL = 0,
  IHS_TRANSFORM_90 = 1,
  IHS_TRANSFORM_180 = 2,
  IHS_TRANSFORM_270 = 3,
  IHS_TRANSFORM_FLIPPED = 4,
  IHS_TRANSFORM_FLIPPED_90 = 5,
  IHS_TRANSFORM_FLIPPED_180 = 6,
  IHS_TRANSFORM_FLIPPED_270 = 7
} IhsTransform;

/* What a layer shows, as a hint for placement (wp_content_type_v1's values). */
typedef enum IhsContentType {
  IHS_CONTENT_TYPE_NONE = 0,
  IHS_CONTENT_TYPE_PHOTO = 1,
  IHS_CONTENT_TYPE_VIDEO = 2,
  IHS_CONTENT_TYPE_GAME = 3
} IhsContentType;

/* The most layers one ihs_pv_submit_layers call may carry. */
#define IHS_PV_MAX_LAYERS 8

/*
 * One layer of a view: a buffer, the part of it shown, where in the view it
 * lands, and how it is oriented. struct_size-first; fields are only ever
 * appended.
 *
 * @frame is the buffer, exactly as ihs_pv_submit takes it; its buffer_id keys
 * the import cache across every layer of the view, and its fds are consumed.
 * @acquire_fence_fd signals when the buffer is ready (-1: it already is) and
 * is consumed too.
 *
 * @layer_id names the layer across submits (a Wayland subsurface keeps one for
 * its life). The shell keys per-layer state on it, so an id that persists
 * keeps its place -- and, where planes are used, its plane -- while the list
 * around it changes.
 *
 * @src_* is the part of the buffer shown, in the buffer's own pixel grid
 * (before @transform), in 16.16 fixed point. @src_w or @src_h of 0 means the
 * whole buffer.
 *
 * @dst_* is where it lands, in view-local physical pixels (the space
 * IhsPvCallbacks::resize reports). @dst_w or @dst_h of 0 means the whole view.
 * A layer that reaches outside the view is clipped to it.
 *
 * @transform is an IhsTransform. @opaque says every pixel of @dst is covered
 * and the buffer's alpha channel is to be ignored (it is undefined in an XRGB
 * buffer). @content_type is an IhsContentType.
 */
typedef struct IhsLayer {
  size_t struct_size;
  const IhsFrame* frame;
  int acquire_fence_fd;
  uint32_t layer_id;
  int32_t src_x;  /* 16.16 */
  int32_t src_y;  /* 16.16 */
  uint32_t src_w; /* 16.16 */
  uint32_t src_h; /* 16.16 */
  int32_t dst_x;
  int32_t dst_y;
  uint32_t dst_w;
  uint32_t dst_h;
  uint32_t transform;   /* IhsTransform */
  uint8_t opaque;       /* 0 or 1 */
  uint8_t content_type; /* IhsContentType */
  uint8_t reserved[2];  /* must be 0 */
} IhsLayer;

/*
 * Replace @view's contents with @layer_count layers, bottom to top.
 *
 * What ihs_pv_submit is for one full-view buffer, this is for a view built
 * from several -- a Wayland toplevel with subsurfaces or popups -- or for one
 * buffer shown cropped, placed within the view, or rotated. ihs_pv_submit is
 * the one-layer case of it: layer 0, the whole buffer, the whole view,
 * NORMAL.
 *
 * Each call is the complete list: a layer_id missing from it is gone, and its
 * buffer is released. 0 layers shows nothing. At most IHS_PV_MAX_LAYERS.
 *
 * @seq is a producer-chosen frame number, reserved for presentation feedback
 * to echo back; pass 0 when unused.
 *
 * @out_release_fence_fds, when not NULL, holds @layer_count entries that
 * receive, per layer, what ihs_pv_submit's out_release_fence_fd would: a fence
 * (owned by the caller) that fires when that layer's buffer may be reused, or
 * -1.
 *
 * FD OWNERSHIP is ihs_pv_submit's, per layer: every plane fd and acquire fence
 * is consumed whatever this returns, except when the list is rejected as
 * malformed (NULL @view, NULL @layers with a non-zero count, a count above
 * IHS_PV_MAX_LAYERS, a layer with a bad struct_size or no frame, or a frame
 * whose struct_size does not reach buffer_id), which closes nothing.
 *
 * Callable from any thread, under the same dispose rule as ihs_pv_submit.
 * Returns IHS_PV_OK, IHS_PV_ERR_INVALID, IHS_PV_ERR_NO_REGISTRY with no host,
 * or IHS_PV_ERR_NO_BACKEND when the host predates this call.
 *
 * Added after 1.0; IHS_WEAK_IMPORT, so test the symbol before calling it.
 */
IHS_EXPORT IHS_WEAK_IMPORT int ihs_pv_submit_layers(IhsPlatformView* view,
                                                    const IhsLayer* layers,
                                                    size_t layer_count,
                                                    uint64_t seq,
                                                    int* out_release_fence_fds);

/*
 * The platform-view capability sub-table reachable through
 * IhsApi::platform_view. Pointers alias the flat entry points above; a consumer
 * may use either. Grows additively behind struct_size.
 */
typedef struct IhsPlatformViewApi {
  size_t struct_size;
  int (*query_capabilities)(IhsPvCapabilities* out);
  /* Native backend context — call the one you need; it returns
     IHS_PV_ERR_UNSUPPORTED when the active backend does not provide it. */
  int (*vulkan_context)(IhsVulkanContext* out);
  int (*egl_context)(IhsEglContext* out);
  int (*register_factory)(const char* view_type,
                          IhsPvFactory factory,
                          void* factory_user_data);
  void (*unregister_factory)(const char* view_type);
  int (*negotiate)(IhsPlatformView* view,
                   const IhsPvRequirements* requirements,
                   IhsPvGrant* out);
  uint32_t (*grant_drm_plane_id)(IhsPlatformView* view);
  int (*grant_shm_fd)(IhsPlatformView* view, size_t* out_stride);
  int (*submit)(IhsPlatformView* view,
                const IhsFrame* frame,
                int acquire_fence_fd,
                int* out_release_fence_fd);

  /* Appended after the initial layout; check struct_size before calling. */
  const char* (*assets_path)(void);
  int (*post_platform_task)(IhsPvTaskFn fn, void* user_data);
  int (*is_platform_thread)(void);
  int (*retire_buffer)(IhsPlatformView* view, uint32_t buffer_id);
  int (*submit_layers)(IhsPlatformView* view,
                       const IhsLayer* layers,
                       size_t layer_count,
                       uint64_t seq,
                       int* out_release_fence_fds);
} IhsPlatformViewApi;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* IHS_PLATFORM_VIEW_H_ */
