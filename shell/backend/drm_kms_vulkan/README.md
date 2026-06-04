# drm_kms_vulkan backend

Drives the Flutter **Vulkan** renderer and presents the result on hardware
KMS planes via **zero-copy dma-buf import**, using the drm-cxx scene
abstraction. The session / modeset / seat / input stack is shared with the
`drm_kms_egl` backend (those translation units sit below the pixel layer and
are GL-free); only the renderer and the present path differ.

Enable with `-DBUILD_BACKEND_DRM_KMS_VULKAN=ON`. **`-DBUILD_COMPOSITOR=ON` is
required** — the engine only reaches the present path through the Flutter
compositor's `CreateBackingStore` / `PresentLayers` callbacks.

## How it works

1. **Device bring-up.** Creates a Vulkan 1.1 instance and selects a physical
   device that can do zero-copy dma-buf scanout: render-node-aware when the
   device exposes a DRM node, vendor/name fallback otherwise (this is what
   picks the real GPU over a software ICD such as llvmpipe). The device is
   gated on **Vulkan 1.1** (the Flutter engine's Skia backend requires a 1.1
   device — a 1.0 device is rejected here so the backend refuses with a clear
   reason instead of the engine fatally aborting), a fixed set of extensions,
   and `timelineSemaphore`; if any are missing the backend refuses cleanly
   rather than handing back a backend that cannot present. Required device
   extensions:
   `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`,
   `VK_EXT_image_drm_format_modifier`, `VK_EXT_queue_family_foreign`,
   `VK_KHR_external_semaphore_fd`, `VK_KHR_external_fence_fd`,
   `VK_KHR_synchronization2`.

2. **Scanout target.** Opens the KMS device read-only, finds a connected
   connector + its CRTC + primary plane, picks a mode (connector-preferred by
   default; see `--drm-mode`), and reads that plane's `IN_FORMATS` modifiers.
   `NegotiateModifiers` intersects the modifiers the GPU can export for the
   color format with those the plane can scan out.

3. **Backing stores.** Each Flutter backing store is a device-local `VkImage`
   allocated with an explicit DRM format modifier (`VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT`)
   and exported as a dma-buf. The image is handed to the engine already in
   `COLOR_ATTACHMENT_OPTIMAL`, which is the layout the Flutter Vulkan renderer
   expects on entry and leaves it in after rendering. `FlutterVulkanImage`
   only carries `{image, format}`, so all backing-store images use the
   vendored Vulkan headers and a single consistent dispatch.

4. **Present.** `avoid_backing_store_cache` is on, so the engine cycles through
   a small **ring of scanout buffers** (rendering into a free buffer while KMS
   scans another). A single persistent drm-cxx `LayerScene` layer presents
   whichever ring slot is ready; the framebuffer is imported once per slot and
   reused. Before each commit a barrier flushes the renderer's color writes
   (`COLOR_ATTACHMENT_OPTIMAL` → `GENERAL`) so the scanout engine sees the
   frame. Commits are **vsync-paced**: the first is a blocking modeset, the
   rest are non-blocking page flips drained against the DRM fd, giving
   tear-free triple-buffered present.

   The Vulkan renderer config also supplies `get_next_image` / `present_image`
   callbacks. They are never invoked on the compositor path, but the embedder
   rejects a Vulkan renderer config that leaves them null, so they are stubs.

## Hardware support

Zero-copy scanout requires a buffer that **both** the render GPU and the
display controller can use directly. What matters is whether the **display**
can address the render GPU's exported (MMU-mapped, possibly scattered) buffer —
either because it is the same device, or because the display has its own
address translation.

| Platform | Render → display | Result |
| --- | --- | --- |
| amdgpu (and unified-memory GPUs) | one device, unified memory + IOMMU | **Works** — validated, renders correctly, tear-free |
| Raspberry Pi 5 | `v3d` (V3D 7.x) → `vc4-drm`, display can address V3D buffers | **Works** — validated, renders correctly, tear-free at 3840×2160 |
| Raspberry Pi 4 | `v3d` (V3D 4.2) → `vc4-drm`, **no display IOMMU** | **Fails** — `AddFB` EINVAL (see below) |
| Arduino Uno Q | Turnip Adreno 702 via virtio/gfxstream → `msm_dpu` | **Fails** — device exposes Vulkan 1.0 (engine needs 1.1); virtualized GPU |
| NanoPC-T6 (rk3588) | Mali-G610 (blob, Vulkan 1.3) → `rockchip-drm` (vop2) | **Refuses** — vop2 rejects the LINEAR import at any resolution; its planes want AFBC, which the LINEAR-only source can't produce |
| BeaglePlay (AM625) | PowerVR AXE-1-16M (Mesa `pvr`, Vulkan 1.2) → `tidss` display | **Refuses** — Mesa `pvr` does not implement `VK_EXT_image_drm_format_modifier` (nor `synchronization2`), so there is no modifier image to export for scanout |

### Unified-memory GPUs (render device == scanout device)

On GPUs where one device both renders and scans out (e.g. **amdgpu**), the
exported modifier image is directly scannable. This is the simplest path.

### Split render/display SoCs

These have a separate render GPU and display controller (e.g. the Pi's `v3d` +
`vc4-drm`). The render GPU has its own MMU and backs `VkImage` allocations with
ordinary **shmem (scattered) pages**. Whether that buffer can be scanned out
depends on the **display** side:

- **Pi 5 (works).** The VideoCore VII display path can address the
  V3D-allocated buffer, so importing the V3D-exported dma-buf as a `vc4`
  framebuffer succeeds and it presents at 4K.
- **Pi 4 (fails).** The `vc4` display HVS has **no IOMMU**; it DMAs the
  framebuffer by physical address and can only scan **physically contiguous
  (CMA)** memory. Registering the scattered V3D buffer as a `vc4` framebuffer
  (`drmModeAddFB2WithModifiers`) returns **EINVAL**. This is independent of
  resolution (confirmed at 3840×2160 and 1920×1080) and of the format/modifier
  (`vc4` advertises `LINEAR` `XRGB8888` and it is negotiated correctly) — the
  buffer's physical layout simply is not scannable.
- **NanoPC-T6 / rk3588 (refuses).** A second failure mode, independent of
  contiguity: the `rockchip-drm` vop2 display *has* an IOMMU, but its planes
  want a tiled/compressed **AFBC** modifier and reject the `LINEAR` import at
  any resolution. The render GPU (Mali-G610 blob) and the source only offer
  `LINEAR`, so there is no common scanout modifier. The display advertising an
  IOMMU is necessary but not sufficient — it must also accept a modifier the
  render GPU can export. The backend refuses cleanly here.
- **BeaglePlay / AM625 (refuses).** A third, earlier failure mode: the render
  GPU's Vulkan driver doesn't implement the modifier extension *at all*. The
  PowerVR AXE-1-16M runs the upstream Mesa `pvr` driver — a real hardware
  Vulkan 1.2 device that *does* expose `external_memory_dma_buf`,
  `queue_family_foreign`, `external_memory_fd` and `timeline_semaphore` — but it
  does **not** implement `VK_EXT_image_drm_format_modifier` (nor
  `synchronization2`). With no modifier extension there is no explicit-modifier
  export image to allocate, so nothing can be negotiated against the `tidss`
  display's `IN_FORMATS` and the gate refuses before rendering. This is a
  driver-maturity gap (Mesa `pvr` is brand new), not a hardware limit. Confirmed
  on-target with `drm_kms_vulkan_probe`: `ZERO-COPY GATE: REFUSE … PowerVR
  A-Series AXE-1-16M missing VK_EXT_image_drm_format_modifier`. The GL backend
  (`drm_kms_egl`) renders fine on this board.

**On the Pi 4, the `cma=` boot setting does not fix this.** CMA *size* governs
how much contiguous memory `vc4` can allocate for its **own** buffers; it has no
effect on where V3D allocates a `VkImage`. V3D never uses CMA (it has an MMU),
so its buffers are non-contiguous regardless of CMA capacity — the mismatch is
the allocation **source/layout**, not the CMA **size**.

Making the export-based path work on a no-display-IOMMU SoC like the Pi 4 would
require inverting the buffer ownership: allocate the scanout buffer from a
**contiguous source** the display can scan (a GBM BO with `GBM_BO_USE_SCANOUT`,
or the kernel CMA dma-buf heap `/dev/dma_heap/linux,cma`) and **import** it into
the Vulkan driver via `VK_EXT_external_memory_dma_buf`, rendering into the
imported buffer. That is an import-based allocator, not implemented here and not
a boot-config change. For such SoCs the GL backend (`drm_kms_egl`, which uses
GBM scanout buffers) is the working path today.

### Vulkan version and virtualized GPUs

Independent of scanout, the device must expose **Vulkan 1.1** (the engine's
Skia backend requires it). Some stacks expose only 1.0 and are rejected at the
gate. The confirmed example is the **Arduino Uno Q**, whose Adreno 702 is
driven by a **virtio-gpu / gfxstream** Turnip (the board runs Linux over a
Qualcomm hypervisor): it reports Vulkan 1.0, so the backend refuses before the
engine starts. The scanout side there is otherwise fine — the `msm_dpu` display
has an SMMU and advertises `LINEAR`, so a native ≥1.1 Adreno/Turnip would be
expected to work.

## Running

The backend needs **DRM master** on the scanout device.

- `--drm-device <node>` — the KMS device with the connectors (the display
  controller, e.g. `card1` for vc4 on a Pi, not the render-only `v3d` node).
- `--drm-mode <W>x<H>[@<R>]` — select the scanout mode (`R` = integer refresh
  Hz; omitted = any). Unset uses the connector's preferred mode.

Session / DRM master:

- With a libseat seat (logind) the backend uses it automatically
  (`[DrmDisplay] libseat session active`).
- Without a usable seat (e.g. a plain SSH login or a console login with no
  logind seat), libseat's in-process builtin backend cannot grab the VT. Force
  the legacy direct-master path by setting **`LIBSEAT_BACKEND=seatd`** with no
  seatd daemon running: libseat fails to open the seat, `DrmSession::Open()`
  returns null, and the backend falls back to a direct `/dev/dri` open +
  `drmSetMaster`. Run from an active VT or as root.

Example (Pi, forcing the fallback and a specific mode):

```sh
LIBSEAT_BACKEND=seatd LD_LIBRARY_PATH=<bundle>/lib \
    homescreen --drm-device /dev/dri/card1 --drm-mode 1920x1080@60 -b <bundle>
```

The Vulkan loader and an ICD must be present at runtime (`libvulkan1` +
`mesa-vulkan-drivers` for V3DV on the Pi; the headers are vendored, so the
loader is dlopen'd, never linked).

## Cross-compiling for the Raspberry Pi

`scripts/build_pi.sh` builds the backend for aarch64:

```sh
scripts/build_pi.sh --backend drm-kms-vulkan --pios trixie --target generic
```

It reuses the `drm-kms-egl` sysroot/toolchain (same DRM/GBM/seat deps; the
Vulkan entry points are dlopen'd, so no link-time `libvulkan`). A JIT (debug)
bundle works for testing: `flutter build bundle --debug` produces an
architecture-independent `kernel_blob.bin` that runs on the target's matching
debug engine.
