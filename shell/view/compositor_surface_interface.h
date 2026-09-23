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
#include <vector>

#include <shell/platform/embedder/embedder.h>

#include "view/layer_geometry.h"

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
    // Which dma-buf @c buffer_id names. A producer may retire an id and later
    // submit it again for different memory; each such reuse is a new
    // generation. The plane path caches a framebuffer per buffer, so it keys
    // that cache on @c ScanoutKey(buffer_id, generation) rather than the id,
    // and a reused id is imported afresh instead of scanning out through the
    // framebuffer of the memory it used to name.
    uint32_t generation{0};
  };

  /**
   * @brief The key a plane path caches a buffer's framebuffer under, and hands
   * back through @c OnScanoutKeyRelease.
   *
   * The buffer_id in the low 32 bits and the generation above them. Where a
   * pointer is 32 bits wide there is no room for the generation, so the key is
   * the id alone (see @c kScanoutKeyHasGeneration), and a surface there must
   * not offer a buffer past generation 0 to a plane: its key would name the
   * old memory's framebuffer.
   */
  static constexpr bool kScanoutKeyHasGeneration =
      sizeof(std::uintptr_t) >= sizeof(uint64_t);
  [[nodiscard]] static constexpr std::uintptr_t ScanoutKey(
      const uint32_t buffer_id,
      const uint32_t generation) {
    if constexpr (kScanoutKeyHasGeneration) {
      return static_cast<std::uintptr_t>(
          (static_cast<uint64_t>(generation) << 32U) | buffer_id);
    } else {
      (void)generation;
      return static_cast<std::uintptr_t>(buffer_id);
    }
  }
  [[nodiscard]] static constexpr uint32_t ScanoutKeyBufferId(
      const std::uintptr_t key) {
    return static_cast<uint32_t>(key & 0xffffffffU);
  }

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
   * @brief The plane path's form of @c OnScanoutRelease: the frame it stopped
   * scanning out, named by the @c ScanoutKey it was cached under.
   *
   * The key says which generation of the buffer left the plane, which the id
   * alone cannot: once a retired id has been submitted again, a late release
   * of its old memory must not hand back the new frame. The default drops the
   * generation and forwards, which is right for a surface that never reuses a
   * retired id.
   */
  virtual void OnScanoutKeyRelease(const std::uintptr_t key) {
    OnScanoutRelease(ScanoutKeyBufferId(key));
  }

  /**
   * @brief Keys of buffers the producer has retired since the last call, so the
   * plane path can drop the framebuffers it cached for them.
   *
   * Nothing depends on this for correctness -- a reused id has a new key -- but
   * without it a retired buffer's framebuffer and dma-buf stay referenced until
   * the cache's own bound evicts them. The default has nothing to report.
   * Compositor thread.
   */
  [[nodiscard]] virtual std::vector<std::uintptr_t> TakeRetiredScanoutKeys() {
    return {};
  }

  /**
   * @brief Confirm the frame @c GetDmabuf handed out was taken for scanout.
   *
   * @c GetDmabuf is deliver-once, but handing a frame over is not the same as
   * taking it: the import can still fail afterwards (pool create, add_layer,
   * replace_source). Committing "delivered" at hand-off means such a frame is
   * never re-offered, so a producer that does not submit again -- a static one
   * -- never returns to scanout.
   *
   * So a caller that takes a @c kFrame owes the surface exactly one of two
   * answers: this, once the frame belongs to whatever will scan it out and so
   * will report its release, or @c OnScanoutRelease with the same
   * @p buffer_id when it will not. Note the ack is about ownership, not about
   * pixels having been displayed -- a commit that fails later is the scene's
   * to retry, and the buffer's release is still accounted for. Releasing is
   * terminal: the ring slot goes back to the producer, so the frame must not
   * be re-offered after it. Answering neither leaves the slot held; answering
   * both double-counts it.
   *
   * The default ignores it, which is correct for a surface whose @c GetDmabuf
   * never returns @c kFrame.
   */
  virtual void AckDmabufScanout(uint32_t /*buffer_id*/) {}

  /**
   * @brief Whether this surface has content to show, new frame or not.
   *
   * @c kNoNewFrame says nothing about whether the surface has ever produced
   * anything, and the two cases need opposite handling on a present where the
   * view is not yet in the scene. A producer that has not delivered its first
   * frame should be skipped, so its view does not flip the frame to GL on
   * every present until a buffer lands -- startup flicker. A surface that has
   * shown content but has no new frame must still be composited, or it blanks
   * until the producer submits again, which for a static producer is forever.
   *
   * The default is false: a surface that never produces content is never in
   * the second case.
   */
  [[nodiscard]] virtual bool HasContent() const { return false; }

  /**
   * @brief The producer's @c buffer_id for the frame @c GetGlTextureName last
   * bound, or 0 when nothing is bound or the surface does not track it.
   *
   * The GL-composite path has no plane and therefore no scanout retire, so it
   * cannot use @c OnScanoutRelease's natural trigger. It needs this to tell one
   * frame from the next: a buffer is finished with once a newer one has
   * displaced it as the bound texture *and* the flip that sampled it has
   * completed. Without it a GL-composited producer never learns that a ring
   * slot is free, waits out its release timeout on every frame, and runs at a
   * fraction of the display rate for reasons that look like the renderer.
   */
  [[nodiscard]] virtual uint32_t GetGlTextureBufferId() const { return 0; }

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

  // ---- Layers --------------------------------------------------------------
  //
  // A surface draws as one or more layers, bottom to top, each a crop of its
  // own image placed within the surface's rect and possibly rotated (see
  // ihs_pv_submit_layers). The accessors above describe layer 0 drawn whole,
  // which is what every surface was before layers existed; the defaults below
  // say exactly that, so a surface that never overrides them is one full-rect
  // layer, drawn the way it always was.
  //
  // The GPU composite paths draw every layer through these, and the plane
  // paths place each layer on a plane of its own through GetLayerDmabuf.
  // GetDmabuf is the single-layer form: layer 0 drawn whole, so a surface with
  // more than one layer, or a layer 0 that is cropped, placed or rotated,
  // reports kNotScanoutCapable there.

  // Where layer @p index's pixels come from and where they land.
  struct LayerGeometry {
    // The part of the image shown, in its own pixels (before @c transform).
    // Zero width or height: the whole image.
    RectF src;
    // Where it lands, in surface-local physical pixels (the FlutterLayer's size
    // spans the surface). Zero width or height: the whole surface.
    RectI dst;
    BufferTransform transform{BufferTransform::kNormal};
    // Every pixel of @c dst is covered; the image's alpha is to be ignored.
    bool opaque{false};

    // Drawn the way a single full-surface image always was: the whole image
    // across the whole surface, upright. (@c opaque does not change where
    // pixels land, so it does not count.)
    [[nodiscard]] bool IsWhole() const {
      return (src.w <= 0 || src.h <= 0) && (dst.w <= 0 || dst.h <= 0) &&
             transform == BufferTransform::kNormal;
    }
  };

  // A layer's Vulkan image, for the Vulkan composite paths. Returned with the
  // geometry of the very frame it holds, so the two cannot come from different
  // submits.
  struct VulkanLayerImage {
    void* image{nullptr};  // VkImage; null: nothing to draw for this layer
    int32_t width{0};
    int32_t height{0};
    uint32_t format{0};  // VkFormat; 0 = VK_FORMAT_B8G8R8A8_UNORM
    uint32_t ycbcr_model{0};
    uint32_t ycbcr_range{0};
    LayerGeometry geometry;
  };

  // A layer's GL texture, for the GL composite paths, with its geometry.
  struct GlLayerTexture {
    uint32_t name{0};  // GLuint; 0: nothing to draw for this layer
    int32_t width{0};
    int32_t height{0};
    bool external{false};  // bind as GL_TEXTURE_EXTERNAL_OES
    bool top_first{false};
    // The producer's buffer_id for the frame bound, for a deferred release.
    uint32_t buffer_id{0};
    LayerGeometry geometry;
  };

  // A layer's dma-buf, for the plane paths, with what places it.
  struct LayerDmabuf {
    // Filled, and owned by the caller, only when GetLayerDmabuf returns kFrame.
    Dmabuf dmabuf;
    // Names the layer across presents, so it keeps its plane while the layers
    // around it come and go.
    uint32_t layer_id{0};
    // The geometry of the layer's newest frame, and the size of its buffer,
    // whichever state is returned: a layer with no new frame is still placed
    // every present, since the view it sits in can move.
    LayerGeometry geometry;
    uint32_t buffer_width{0};
    uint32_t buffer_height{0};
  };

  // Layer @p index's newest frame as a dma-buf for a plane: GetDmabuf, per
  // layer, with the same deliver-once rule and the same answer owed for a
  // kFrame (AckDmabufScanout or OnScanoutRelease with its buffer_id). The
  // default is GetDmabuf for layer 0 and kNotScanoutCapable for the rest.
  // Compositor thread.
  [[nodiscard]] virtual DmabufState GetLayerDmabuf(const size_t index,
                                                   LayerDmabuf* out) const {
    if (index != 0 || out == nullptr) {
      return DmabufState::kNotScanoutCapable;
    }
    const DmabufState state = GetDmabuf(&out->dmabuf);
    out->layer_id = 0;
    out->geometry = {};
    out->buffer_width = out->dmabuf.width;
    out->buffer_height = out->dmabuf.height;
    return state;
  }

  // How many layers to draw this present. At least 1 for any surface that
  // draws; 0 draws nothing.
  [[nodiscard]] virtual size_t GetLayerCount() const { return 1; }

  // Layer @p index's image and format. Compositor thread.
  [[nodiscard]] virtual VulkanLayerImage GetLayerVulkanImage(
      const size_t index) const {
    VulkanLayerImage out;
    if (index == 0) {
      out.image = GetVulkanImage(&out.width, &out.height);
      out.format = GetVulkanImageFormat();
      out.ycbcr_model = GetVulkanYcbcrModel();
      out.ycbcr_range = GetVulkanYcbcrRange();
    }
    return out;
  }
  [[nodiscard]] virtual uint32_t GetLayerVulkanImageLayout(
      const size_t index) const {
    return index == 0 ? GetVulkanImageLayout() : 0;
  }
  virtual void SetLayerVulkanImageLayout(const size_t index,
                                         const uint32_t layout) {
    if (index == 0) {
      SetVulkanImageLayout(layout);
    }
  }
  // Ownership moves to the caller, as with TakeAcquireFenceFd.
  [[nodiscard]] virtual int TakeLayerAcquireFenceFd(const size_t index) {
    return index == 0 ? TakeAcquireFenceFd() : -1;
  }

  // Layer @p index's texture. Raster thread with the GL context current: like
  // GetGlTextureName, this may import the layer's latest frame.
  [[nodiscard]] virtual GlLayerTexture GetLayerGlTexture(
      const size_t index) const {
    GlLayerTexture out;
    if (index == 0) {
      out.name = GetGlTextureName();
      if (out.name != 0) {
        out.width = GetGlTextureWidth();
        out.height = GetGlTextureHeight();
        out.external = TextureIsExternalOes();
        out.top_first = TextureIsTopFirst();
        out.buffer_id = GetGlTextureBufferId();
      }
    }
    return out;
  }
};
