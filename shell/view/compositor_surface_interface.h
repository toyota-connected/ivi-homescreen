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

#pragma once

#include <unistd.h>
#include <cstdint>

#include <shell/platform/embedder/embedder.h>

/**
 * @brief Plugin-facing interface for a platform-view backing store producer.
 *
 * The compositor owns a registry of @c ICompositorSurface instances keyed by
 * @c FlutterPlatformViewIdentifier. On each frame the compositor dispatches
 * backing-store lifecycle and presentation calls into the matching surface.
 * Implementations must be callable from the rasterizer thread.
 */
class ICompositorSurface {
 public:
  virtual ~ICompositorSurface() = default;

  /**
   * @brief Fill @p store_out with a backing store for this surface's layer.
   *
   * The embedder retains ownership of any GPU resources referenced by the
   * returned store. The engine calls @c OnCollectBackingStore when the store
   * is no longer in use.
   *
   * @return true on success; false causes the engine to fall back to an
   *         engine-owned store for this frame.
   */
  virtual bool OnCreateBackingStore(const FlutterBackingStoreConfig* config,
                                    FlutterBackingStore* store_out) = 0;

  /**
   * @brief Retire a backing store previously produced by this surface.
   */
  virtual bool OnCollectBackingStore(const FlutterBackingStore* store) = 0;

  /**
   * @brief Present a frame for this surface.
   *
   * The compositor has already reconciled the Wayland subsurface Z-order
   * before this call. The implementation should draw into or swap its native
   * surface.
   */
  virtual bool OnPresent(const FlutterLayer* layer) = 0;

  /**
   * @brief The platform view identifier this surface is registered under.
   */
  [[nodiscard]] virtual FlutterPlatformViewIdentifier GetIdentifier() const = 0;

  /**
   * @brief Optional resize notification. Default is a no-op.
   */
  virtual void OnResize(int32_t /*width*/, int32_t /*height*/) {}

  /**
   * @brief OpenGL texture name this plugin renders into.
   *
   * Platform-view plugins on the EGL backend expose their final image
   * via a @c GL_TEXTURE_2D. The compositor composites it onto the scene
   * at the layer's @c offset / @c size during @c PresentLayers. Returning
   * 0 (default) means the plugin handles its own presentation and the
   * compositor performs no compositing for this layer.
   *
   * The texture must be valid in the engine's GL context — i.e., created
   * with that context current (typical pattern: lazy-init in @c OnPresent).
   *
   * Vulkan platform-view sharing is a follow-up; @c VkImage exposure will
   * land alongside the matching backend wiring.
   */
  [[nodiscard]] virtual uint32_t GetGlTextureName() const { return 0; }

  /**
   * @brief Width of the GL texture returned by @c GetGlTextureName.
   */
  [[nodiscard]] virtual int32_t GetGlTextureWidth() const { return 0; }

  /**
   * @brief Height of the GL texture returned by @c GetGlTextureName.
   */
  [[nodiscard]] virtual int32_t GetGlTextureHeight() const { return 0; }

  /**
   * @brief Declare the in-memory Y orientation of the exposed GL texture.
   *
   * The compositor needs this to decide whether to invert V at sample time.
   *
   * - @c false (default) — the texture is GL-native: memory row 0 maps to
   *   texcoord v=0 (bottom in GL convention). Typical for plugins that render
   *   their own content into an FBO-bound texture without a manual flip, which
   *   is what the standard embedder contract assumes.
   * - @c true — memory row 0 corresponds to the visual top of the source
   *   (e.g. NV12 dmabuf imported via EGLImage, or a YUV-to-RGB shader that
   *   was deliberately authored with a `1 - v` sampler flip). The compositor
   *   compensates so the image lands right-side-up on both standard FBO-0 and
   *   the DRM scanout-FBO destinations.
   */
  [[nodiscard]] virtual bool TextureIsTopFirst() const { return false; }

  /**
   * @brief Whether GetGlTextureName() is bound to GL_TEXTURE_EXTERNAL_OES.
   *
   * True for a YUV dma-buf import — planar (e.g. NV12) or packed (e.g.
   * YUYV/UYVY) — which is sampled through samplerExternalOES, where the driver
   * does the YUV->RGB conversion. Sampling such a texture as GL_TEXTURE_2D
   * yields raw/luma data. Packed RGB(A) formats stay on GL_TEXTURE_2D and leave
   * this false.
   */
  [[nodiscard]] virtual bool TextureIsExternalOes() const { return false; }

  /**
   * @brief Expose a Vulkan platform-view image the compositor can composite.
   *
   * The Vulkan counterpart to @c GetGlTextureName: a plugin that rendered into
   * a @c VkImage on the backend's own device (obtained via
   * @c Backend::GetVulkanContext) returns that handle here so a Vulkan
   * compositor can composite it directly — same device, no cross-API/-device
   * import. The layer compositor samples it in a shader
   * (SHADER_READ_ONLY_OPTIMAL), so it composes with alpha rather than a plain
   * overwriting blit. Returned as @c void* so this header stays free of a
   * Vulkan include; the caller casts back to @c VkImage. Fills @p width / @p
   * height with the image extent. Returns nullptr (default) when the plugin has
   * no Vulkan image (GL path, or nothing rendered yet).
   *
   * Contract: the image's memory holds a complete frame (the plugin has made
   * its writes visible) and it is safe to transition to a shader-read layout
   * and read this frame. Its VkFormat is reported by @c GetVulkanImageFormat
   * (a packed RGB format sampled directly, or a planar YUV format sampled
   * through a VkSamplerYcbcrConversion the compositor builds).
   */
  [[nodiscard]] virtual void* GetVulkanImage(int32_t* /*width*/,
                                             int32_t* /*height*/) const {
    return nullptr;
  }

  /**
   * @brief VkFormat (as uint32_t, keeping this header Vulkan-free) of the image
   * returned by @c GetVulkanImage. 0 (VK_FORMAT_UNDEFINED, the default) means
   * the historical B8G8R8A8_UNORM contract, so an RGB producer need not
   * override this. A planar YUV producer returns its multi-planar format (e.g.
   * VK_FORMAT_G8_B8R8_2PLANE_420_UNORM), which the compositor samples through a
   * VkSamplerYcbcrConversion built from @c GetVulkanYcbcrModel /
   * @c GetVulkanYcbcrRange.
   */
  [[nodiscard]] virtual uint32_t GetVulkanImageFormat() const { return 0; }

  /**
   * @brief For a YUV @c GetVulkanImage, the VkSamplerYcbcrModelConversion and
   * VkSamplerYcbcrRange (as uint32_t) the compositor's conversion must apply,
   * resolved by the producer from the frame's color metadata. Consulted only
   * when @c GetVulkanImageFormat reports a YUV format; a producer that reports
   * one MUST override both (there is no color metadata to default from here).
   *
   * The defaults are the values a non-overriding (RGB) producer yields and are
   * never sampled through a conversion: 0 is
   * VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY for the model and
   * VK_SAMPLER_YCBCR_RANGE_ITU_FULL for the range.
   */
  [[nodiscard]] virtual uint32_t GetVulkanYcbcrModel() const { return 0; }
  [[nodiscard]] virtual uint32_t GetVulkanYcbcrRange() const { return 0; }

  /**
   * @brief Current VkImageLayout of the Vulkan image (as uint32_t, so this
   * header stays Vulkan-free). The compositor reads this to transition the
   * image from its real layout, and calls @c SetVulkanImageLayout after doing
   * so — a static platform-view image is then transitioned once, not per frame.
   * 0 == VK_IMAGE_LAYOUT_UNDEFINED.
   */
  [[nodiscard]] virtual uint32_t GetVulkanImageLayout() const { return 0; }
  virtual void SetVulkanImageLayout(uint32_t /*layout*/) {}

  /**
   * @brief A single dma-buf frame a surface can hand to a KMS overlay plane.
   *
   * Plain data so this header stays free of DRM/GBM includes. Multi-planar
   * (YUV) frames fill @c plane_count entries of @c fd / @c offset / @c stride,
   * one per plane. A single-handle layout (v4l2/libcamera contiguous,
   * Chromium's accelerated paint) repeats the same handle in every @c fd entry
   * with the planes separated by @c offset; a multi-handle layout (e.g. the
   * rpi-hevc-dec, which exports the Y and C planes as distinct dma-bufs) fills
   * each @c fd with its own handle. AddFB2 resolves each fd to a GEM handle, so
   * both cases scan out. GetDmabuf transfers ownership of every populated @c fd
   * (all @c plane_count of them, not just @c fd[0]) to the caller, which must
   * close each after importing the frame. @c fourcc is a @c DRM_FORMAT_* code
   * and @c modifier a @c DRM_FORMAT_MOD_* value — @c DRM_FORMAT_MOD_LINEAR (0)
   * for a plain linear buffer, @c DRM_FORMAT_MOD_INVALID only when the modifier
   * is unknown/unspecified (let the importer infer). @c width / @c height are
   * the source extent in pixels; the plane scales this to the layer's dst rect.
   *
   * @c acquire_fence_fd, when >= 0, is a sync_file naming when the producer's
   * writes complete — intended for the atomic commit's @c IN_FENCE_FD so
   * scanout is tear-free without a CPU stall. The DRM scene path imports it and
   * hands it to the plane source (wired to @c IN_FENCE_FD, with a CPU wait as a
   * fallback on drivers lacking that property); leave it -1 for a producer that
   * has already synced. Ownership stays with the surface — the compositor dups
   * what it needs.
   */
  struct Dmabuf {
    int fd[4]{-1, -1, -1, -1};  // one owned handle per plane (see above)
    uint32_t fourcc{0};
    uint64_t modifier{0};
    uint32_t width{0};
    uint32_t height{0};
    uint32_t plane_count{0};
    uint32_t offset[4]{};
    uint32_t stride[4]{};
    // Container colorimetry of the pixels (IhsColorSpace / IhsColorRange). Only
    // meaningful for YUV frames; the DRM scene path lowers these to the plane's
    // COLOR_ENCODING / COLOR_RANGE CSC. DEFAULT (0) leaves the scene's default.
    uint8_t color_space{0};
    uint8_t color_range{0};
    // HDR static metadata for an HDR (PQ/HLG) video, mirrored from the
    // producer's IhsHdrMetadata. Not carried per-frame on the Dmabuf — the DRM
    // scene path reads the view's persisted value via @c GetHdrMetadata (steady
    // across reuse presents) and lowers it to the connector's
    // HDR_OUTPUT_METADATA. This is the shape @c GetHdrMetadata fills.
    struct HdrMetadata {
      uint8_t transfer{0};                // 0=SDR, 1=PQ, 2=HLG (IhsTransfer)
      uint16_t display_primaries_x[3]{};  // R,G,B in 0.00002 units
      uint16_t display_primaries_y[3]{};
      uint16_t white_point_x{0};
      uint16_t white_point_y{0};
      uint32_t max_display_mastering_luminance{0};  // cd/m^2
      uint32_t min_display_mastering_luminance{0};  // 0.0001 cd/m^2
      uint16_t max_content_light_level{0};
      uint16_t max_frame_average_light_level{0};
    };
    int acquire_fence_fd{-1};
    // The producer's identity for this frame (IhsFrame.buffer_id, its ring
    // slot). The compositor hands it back via OnScanoutRelease when the plane
    // stops scanning the frame out, so the producer can reuse that slot.
    uint32_t buffer_id{0};
  };

  /**
   * @brief Why a present has no dma-buf for the plane, or that it has one.
   *
   * @c GetDmabuf used to answer with a bool, which conflated three cases the
   * compositor has to treat differently. @c kNoNewFrame means keep whatever is
   * already on the plane; @c kNotScanoutCapable means there *is* new content
   * but it cannot go on a plane, so GL-composite it rather than leaving the
   * plane showing a stale frame.
   */
  enum class DmabufState : uint8_t {
    /// No fresh content this present. A reused layer keeps its current source;
    /// a view whose producer has not delivered its first frame yet is simply
    /// not ready. Nothing to do.
    kNoNewFrame = 0,
    /// New content exists but is not expressible as a scanout dma-buf -- a
    /// GL-only surface, a frame with per-plane handles the layout cannot carry,
    /// or a transient failure to dup the producer's fds. The compositor must
    /// route this view through GL for the present; holding the plane would
    /// freeze it on the last scannable frame.
    kNotScanoutCapable,
    /// @p out is filled and the caller owns every populated @c fd.
    kFrame,
  };

  /**
   * @brief Expose the surface's latest frame as a dma-buf for direct scanout.
   *
   * Fills @p out and returns @c kFrame when a fresh frame is ready to be placed
   * on a KMS overlay plane; the compositor then routes it onto a plane (the
   * FlutterLayer geometry drives the Layer Crtc and Src rects) instead of
   * GL-compositing it. Otherwise nothing is written to @p out and the state
   * says which of the two no-frame cases applies -- see @c DmabufState, and
   * note that only @c kNoNewFrame is safe to answer by keeping the plane as it
   * is.
   *
   * Deliver-once: a frame is handed to the scanout path at most once, so a
   * reused layer polling every present sees @c kNoNewFrame until the producer
   * submits again. The returned @c fd is owned by the caller (an implementation
   * may dup its internal handle to make it robust against a concurrent producer
   * superseding the frame); the caller closes each one once it has imported the
   * buffer.
   *
   * The default is @c kNotScanoutCapable: a surface that does not override this
   * has no dma-buf path at all, and its content only reaches the screen through
   * @c GetGlTextureName / @c GetVulkanImage.
   */
  [[nodiscard]] virtual DmabufState GetDmabuf(Dmabuf* /*out*/) const {
    return DmabufState::kNotScanoutCapable;
  }

  /**
   * @brief The surface's current HDR static metadata, if its content is HDR.
   *
   * Unlike @c GetDmabuf (deliver-once — valid only on a fresh frame), this
   * reports the view's *persisted* HDR state, so the compositor can hold the
   * connector's HDR_OUTPUT_METADATA steady across presents where the view is
   * merely reused (no new frame). Toggling it per-present would re-sync the
   * sink's HDR mode every few frames — visible flicker. Returns false for SDR
   * content (the default), which clears HDR only when the HDR view actually
   * leaves the scene.
   */
  [[nodiscard]] virtual bool GetHdrMetadata(
      Dmabuf::HdrMetadata* /*out*/) const {
    return false;
  }

  /**
   * @brief Whether the Vulkan image is a dma-buf the producer rewrites on
   * another agent's queue, so the compositor must take ownership of it before
   * reading and hand it back after.
   *
   * - @c false (default) — a static image on the compositor's own device: the
   *   compositor transitions it to a transfer source ONCE (tracked via
   *   Get/SetVulkanImageLayout) and reads it every frame thereafter.
   * - @c true — an imported dma-buf whose content is written by an external
   *   producer through an aliased image. The compositor issues a
   *   VK_QUEUE_FAMILY_EXTERNAL ownership acquire (from GENERAL to transfer
   *   source) before the blit and a release back after it, every frame; the
   *   producer performs the complementary release/acquire around its render.
   *   GetVulkanImageLayout is not consulted on this path.
   */
  [[nodiscard]] virtual bool NeedsExternalQueueAcquire() const { return false; }

  /**
   * @brief Take the acquire fence (a sync_file fd) for the frame the producer
   * most recently submitted, transferring ownership to the caller.
   *
   * When the producer submits asynchronously (explicit sync), it exports a
   * sync_file that signals when its render/copy into the imported dma-buf has
   * completed and hands it over with the frame. The compositor imports it into
   * a VkSemaphore and waits on it before sampling the image, so it need not
   * rely on the producer having stalled on the CPU first. Returns -1 when there
   * is no pending acquire fence (the producer stalled synchronously, or the
   * device lacks SYNC_FD semaphore support); the caller then reads the image
   * directly. The returned fd is owned by the caller (import-consumes or close
   * it).
   */
  [[nodiscard]] virtual int TakeAcquireFenceFd() { return -1; }

  /**
   * @brief Hand back a release fence (a sync_file fd) that signals when the
   * frame the compositor just submitted has finished sampling this surface's
   * image, transferring ownership of the fd.
   *
   * The producer waits on it before overwriting a ring buffer, and the host
   * waits on it before freeing an import at dispose, so neither frees or
   * rewrites memory the compositor is still reading. The default ignores it
   * (closing the fd) for surfaces that manage their own lifetime. The fd is
   * owned by the callee.
   */
  virtual void SetReleaseFenceFd(int fd) {
    if (fd >= 0) {
      ::close(fd);
    }
  }

  /**
   * @brief The plane has stopped scanning out the frame the producer submitted
   * with @p buffer_id (Dmabuf::buffer_id); that ring slot is free to reuse.
   *
   * The DRM scene path calls this once per submitted frame when the frame is
   * retired (a later frame replaced it on the plane) -- normally on the
   * compositor thread, but scene/pool teardown (e.g. ~DrmCompositor draining
   * in-flight buffers) can call it from another thread, so an implementation
   * must be thread-safe. It is the per-buffer complement of the per-submit
   * @c SetReleaseFenceFd: on the plane path a producer waits on the release
   * fence handed back at submit, so a surface backing that path signals that
   * fence here. The default ignores it (surfaces whose producer does not track
   * per-buffer releases).
   */
  virtual void OnScanoutRelease(uint32_t /*buffer_id*/) {}

  /**
   * @brief Report which KMS plane the surface's frame was scanned out on this
   * present, or 0 when it was GL-composited (no plane) this present.
   *
   * The DRM scene path calls this once per present after the atomic commit,
   * with the plane object id the allocator placed this surface on (or 0 on a
   * GL-composite fallback). It feeds the @c DRM_PLANE grant accessor
   * (@c ihs_pv_grant_drm_plane_id), so a direct-scanout producer can tell
   * whether its zero-GPU path is actually being honored frame to frame. Called
   * on the compositor thread; an implementation must be thread-safe. The
   * default ignores it.
   */
  virtual void SetScanoutPlane(uint32_t /*plane_id*/) {}
};
