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

// ihs-vk-export wire protocol, version 1 -- the contract between the bridge
// (server) and a CARLA/Unreal consumer (client) over a Unix SEQPACKET socket.
//
// Framing: every message is one seqpacket datagram = a fixed little-endian
// header { u32 type; u32 len } immediately followed by `len` payload bytes.
// File descriptors (memory + semaphore fds) never appear in the payload; they
// ride the SAME datagram as ancillary SCM_RIGHTS data, in the well-defined
// order documented per message. The receiver dup()s and owns every fd it gets.
//
// Endianness/ABI: fields are little-endian and the payload structs are shared
// verbatim by both ends (same header, same arch). Every struct is padding-free
// by construction (fields ordered widest-first) and guarded with a
// static_assert on its size so an accidental layout change is caught at compile
// time.
//
// Roles: the bridge is the server (it owns the exported image pool and drives
// presentation); the consumer connects, matches the advertised deviceUUID, and
// imports the pool. Directions below are B=bridge, C=consumer.

#ifndef IHS_CARLA_BRIDGE_WIRE_PROTOCOL_H_
#define IHS_CARLA_BRIDGE_WIRE_PROTOCOL_H_

#include <cstdint>

namespace ihs_vke {

inline constexpr uint32_t kProtocolVersion = 1;
inline constexpr uint32_t kMaxSlots =
    8;  // pool images per session (pool is small)
inline constexpr uint32_t kMaxPlanes = 4;  // DRM planes per dmabuf image
inline constexpr uint32_t kConsumerNameMax = 64;

// Default socket path (the deployment may override). $XDG_RUNTIME_DIR is
// resolved at runtime; this is the leaf name.
inline constexpr char kDefaultSocketLeaf[] = "ihs-vk-export.sock";

enum class MsgType : uint32_t {
  kHello = 1,       // C->B: version, consumer name, consumer deviceUUID
  kCaps = 2,        // B->C: version, deviceUUID, offered handle types, format,
                    //       extent, pool size, capability flags
  kImageTable = 3,  // B->C: per-slot descriptors + fds (mem,render_done,
                    //       consumer_done) via SCM_RIGHTS; carries a generation
  kFramePresent = 4,  // B->C: slot index + frame sequence + damage rect (info)
  kFrameRelease = 5,  // C->B: slot index (consumer_done was signaled GPU-side;
                      //       this is the CPU-side pacing edge)
  kInputPointer = 6,  // C->B: pointer phase, x, y (logical px), buttons, device
  kInputKey = 7,      // C->B: reserved for v1 (unimplemented)
  kResize = 8,        // C->B: new extent -> pool rebuild + new kImageTable
  kBye = 9,           // either: reason code
};

// Handle-type bits a session can carry (mirror VkExternalMemoryHandleType).
enum HandleTypeBits : uint32_t {
  kHandleOpaqueFd = 1u << 0,  // same-device OPAQUE_FD (the portable floor)
  kHandleDmaBuf = 1u << 1,    // dma-buf with a DRM format modifier
};

// Capability flags (Caps.flags).
enum CapsFlagBits : uint32_t {
  kCapsTimelineSemaphores = 1u << 0,  // timeline sems available (else binary)
};

// ---- Fixed message header (precedes every payload) --------------------------
struct MsgHeader {
  uint32_t type;  // MsgType
  uint32_t len;   // payload byte count following this header
};
static_assert(sizeof(MsgHeader) == 8, "MsgHeader must be 8 bytes");

// ---- Payloads ---------------------------------------------------------------

// C->B. The consumer announces itself and the GPU it will import onto; the
// bridge must have selected the SAME physical device (deviceUUID match) for an
// OPAQUE_FD import to be valid.
struct Hello {
  uint32_t protocol_version;
  uint32_t consumer_name_len;  // bytes of name that follow the struct (<= max)
  uint8_t device_uuid[16];     // VkPhysicalDeviceIDProperties.deviceUUID
  // followed by consumer_name_len bytes of UTF-8 name (not NUL-terminated)
};
static_assert(sizeof(Hello) == 24, "Hello layout");

// B->C. What the pool advertises. handle_types is a HandleTypeBits mask; the
// consumer picks one it can import and expects the following kImageTable to use
// it. extent + slot_count + vk_format describe the pool shape up front.
struct Caps {
  uint32_t protocol_version;
  uint32_t handle_types;  // HandleTypeBits mask offered
  uint32_t vk_format;     // VkFormat of every pool image
  uint32_t drm_fourcc;    // DRM fourcc (dmabuf); 0 if opaque-only
  uint32_t width;
  uint32_t height;
  uint32_t slot_count;  // pool images (<= kMaxSlots)
  uint32_t flags;       // CapsFlagBits
  uint8_t device_uuid[16];
};
static_assert(sizeof(Caps) == 48, "Caps layout");

// One pool image in a kImageTable. The three fds for this slot -- memory,
// render_done semaphore, consumer_done semaphore -- travel as SCM_RIGHTS in the
// same datagram; see kImageTable's fd order below.
struct ImageDesc {
  uint64_t alloc_size;                // total device memory bytes
  uint64_t drm_modifier;              // dmabuf DRM_FORMAT_MOD_*; 0 if opaque
  uint64_t plane_offset[kMaxPlanes];  // dmabuf per-plane byte offset
  uint64_t plane_pitch[kMaxPlanes];   // dmabuf per-plane row stride
  uint32_t width;
  uint32_t height;
  uint32_t vk_format;          // VkFormat
  uint32_t drm_fourcc;         // dmabuf; 0 if opaque
  uint32_t memory_type_index;  // for an opaque-fd import
  uint32_t handle_type;        // HandleTypeBits (single bit) used for this pool
  uint32_t plane_count;        // dmabuf plane count (1 for opaque)
  uint32_t reserved;           // pad to 8-byte multiple; must be 0
};
static_assert(sizeof(ImageDesc) == 16 + 2 * 8 * kMaxPlanes + 32,
              "ImageDesc layout");

// B->C header for kImageTable. Followed by `slot_count` ImageDesc records.
// SCM_RIGHTS fds accompany the datagram in slot order, three per slot:
//   [ mem_0, render_done_0, consumer_done_0, mem_1, render_done_1, ... ]
// i.e. 3 * slot_count fds. `generation` bumps on every rebuild (see kResize);
// the consumer must drop a stale-generation table.
struct ImageTableHeader {
  uint32_t generation;
  uint32_t slot_count;
  // followed by slot_count ImageDesc records
};
static_assert(sizeof(ImageTableHeader) == 8, "ImageTableHeader layout");

// B->C. A frame was presented into `slot`. damage_* is informational (whole
// image is valid); the consumer waits the slot's render_done semaphore GPU-side
// before sampling.
struct FramePresent {
  uint32_t slot;
  uint32_t frame_seq;
  uint32_t generation;  // must match the current kImageTable
  int32_t damage_x, damage_y, damage_w, damage_h;
};
static_assert(sizeof(FramePresent) == 28, "FramePresent layout");

// C->B. The consumer released `slot` (its consumer_done semaphore was signaled
// GPU-side). This is the CPU-side pacing edge the backend's vsync consumes.
struct FrameRelease {
  uint32_t slot;
  uint32_t generation;
};
static_assert(sizeof(FrameRelease) == 8, "FrameRelease layout");

enum class PointerPhase : uint32_t {
  kAdd = 0,
  kDown = 1,
  kMove = 2,
  kUp = 3,
  kRemove = 4,
};

// C->B. Pointer input in the bridge's logical pixel space; the shell injects it
// through the engine's normal pointer path (hit-testing/hover/a11y correct).
struct InputPointer {
  uint32_t phase;    // PointerPhase
  float x, y;        // logical pixels
  uint32_t buttons;  // button bitmask
  uint32_t device_id;
};
static_assert(sizeof(InputPointer) == 20, "InputPointer layout");

// C->B. New extent; the bridge rebuilds the pool and sends a fresh kImageTable
// with an incremented generation.
struct Resize {
  uint32_t width;
  uint32_t height;
};
static_assert(sizeof(Resize) == 8, "Resize layout");

enum class ByeReason : uint32_t {
  kNormal = 0,
  kProtocolError = 1,
  kDeviceMismatch = 2,  // consumer could not match the advertised deviceUUID
  kUnsupported = 3,     // no mutually supported handle type
  kInternalError = 4,
};

// Either direction. Best-effort last message before closing.
struct Bye {
  uint32_t reason;  // ByeReason
};
static_assert(sizeof(Bye) == 4, "Bye layout");

// Number of SCM_RIGHTS fds a kImageTable carries for `slot_count` slots.
inline constexpr uint32_t ImageTableFdCount(uint32_t slot_count) {
  return 3u * slot_count;  // mem + render_done + consumer_done per slot
}

}  // namespace ihs_vke

#endif  // IHS_CARLA_BRIDGE_WIRE_PROTOCOL_H_
