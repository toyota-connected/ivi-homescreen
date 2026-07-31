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

// The CANONICAL, shell-side definition of the in-process C ABI the
// ivi-homescreen headless-vulkan backend exposes to a bridge plugin loaded into
// the same process. ivi-homescreen owns this header and exports exactly one
// symbol, `ihs_vk_export_get_api`, at default visibility; the bridge vendors a
// byte-identical copy and resolves the symbol with dlsym(RTLD_DEFAULT) without
// ever linking the shell. Because the seam is in-process there is no IPC and no
// fd passing here -- raw fds/handles cross by value; the bridge is what
// marshals them out over the ihs-vk-export wire protocol (see wire_protocol.h).
//
// Versioned by a single get-api call: `ihs_vk_export_get_api(version)` returns
// a struct-of-function-pointers for the requested major version or NULL. Adding
// a method means a new struct + version, never reordering V1.
//
// This header intentionally mirrors the headless-vulkan backend's exported
// state (ExportedImage / device_uuid) so the bridge can be written and tested
// against it before the shell symbol exists.

#ifndef IHS_VK_EXPORT_H_
#define IHS_VK_EXPORT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IHS_VK_EXPORT_API_VERSION 1
#define IHS_VK_EXPORT_MAX_SLOTS 8
#define IHS_VK_EXPORT_MAX_PLANES 4

// Handle type actually used by the pool (matches VkExternalMemoryHandleType).
enum {
  IHS_VK_HANDLE_OPAQUE_FD = 1,  // same-device OPAQUE_FD (portable floor)
  IHS_VK_HANDLE_DMA_BUF = 2,    // dma-buf + DRM format modifier
};

// Pool-wide capabilities the bridge advertises to a consumer at HELLO/CAPS.
typedef struct IhsVkExportCaps {
  uint32_t api_version;
  uint32_t handle_type;  // one of IHS_VK_HANDLE_*
  uint32_t vk_format;    // VkFormat of every pool image
  uint32_t drm_fourcc;   // DRM fourcc (dmabuf); 0 if opaque
  uint32_t width;
  uint32_t height;
  uint32_t slot_count;     // pool images in the current table
  uint32_t export_active;  // 0 if the backend has no exportable pool
  uint8_t device_uuid[16];
} IhsVkExportCaps;

// One pool image. fds are OWNED BY THE CALLER: get_image_table() fills them
// with dup()s the bridge must close. A slot with no export leaves fds == -1.
typedef struct IhsVkExportImage {
  int32_t memory_fd;         // dup of the image's exported memory fd
  int32_t render_done_fd;    // dup of the render_done semaphore fd
  int32_t consumer_done_fd;  // dup of the consumer_done semaphore fd
  uint64_t alloc_size;
  uint64_t drm_modifier;                            // dmabuf; 0 if opaque
  uint64_t plane_offset[IHS_VK_EXPORT_MAX_PLANES];  // dmabuf
  uint64_t plane_pitch[IHS_VK_EXPORT_MAX_PLANES];   // dmabuf
  uint32_t width;
  uint32_t height;
  uint32_t vk_format;
  uint32_t drm_fourcc;
  uint32_t memory_type_index;
  uint32_t handle_type;  // IHS_VK_HANDLE_*
  uint32_t plane_count;
} IhsVkExportImage;

typedef struct IhsVkExportImageTable {
  uint32_t generation;  // bumps on every pool rebuild (resize)
  uint32_t slot_count;
  IhsVkExportImage images[IHS_VK_EXPORT_MAX_SLOTS];
} IhsVkExportImageTable;

// Fired on the raster thread the instant the backend presents a frame into
// `slot`; the bridge must only enqueue to its own ring here (socket writes
// happen on the bridge IO thread). Not called concurrently with itself.
typedef void (*IhsVkFrameListener)(uint32_t slot,
                                   uint32_t frame_seq,
                                   void* user);

// Pacing source for the backend's vsync (see the consumer-paced vsync source).
enum {
  IHS_VK_PACE_FREE_RUN = 0,         // ceiling-only; no consumer attached
  IHS_VK_PACE_CONSUMER_DRIVEN = 1,  // release_frame() edges drive the cadence
};

typedef struct IhsPointerEvent {
  uint32_t phase;  // add/down/move/up/remove
  float x, y;      // logical pixels
  uint32_t buttons;
  uint32_t device_id;
} IhsPointerEvent;

// V1 function table. All entry points are safe to call from the bridge IO
// thread unless noted; set_frame_listener's callback fires on the raster
// thread.
typedef struct IhsVkExportApiV1 {
  // Pool capabilities (device UUID, handle type, format, extent, slot count).
  IhsVkExportCaps (*get_caps)(void);
  // Fill `out` with the current pool: descriptors + dup'd fds the caller owns.
  // Returns 0 on success, non-zero if there is no exportable pool.
  int (*get_image_table)(IhsVkExportImageTable* out);
  // Register the per-present callback (raster thread). cb == NULL clears it.
  void (*set_frame_listener)(IhsVkFrameListener cb, void* user);
  // The consumer released `slot` -- the pacing edge for consumer-driven vsync.
  void (*release_frame)(uint32_t slot);
  // Choose consumer-driven vs free-run pacing (IHS_VK_PACE_*).
  void (*set_pace_source)(uint32_t src);
  // Inject a pointer event through the engine's normal input path (thread-safe,
  // marshaled to the platform task runner).
  void (*inject_pointer)(const IhsPointerEvent* ev);
  // Rebuild the pool at a new extent (resize). Returns 0 on success.
  int (*rebuild_pool)(uint32_t width, uint32_t height);
} IhsVkExportApiV1;

// The one exported shell symbol. Returns the table for `requested_version`
// (currently only IHS_VK_EXPORT_API_VERSION), or NULL if the running backend is
// not headless-vulkan or the version is unsupported.
const IhsVkExportApiV1* ihs_vk_export_get_api(uint32_t requested_version);

// Convenience typedef for the dlsym'd pointer.
typedef const IhsVkExportApiV1* (*IhsVkExportGetApiFn)(uint32_t);
#define IHS_VK_EXPORT_GET_API_SYMBOL "ihs_vk_export_get_api"

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // IHS_VK_EXPORT_H_
