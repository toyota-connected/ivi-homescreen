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
   * @brief Expose a Vulkan platform-view image the compositor can blit.
   *
   * The Vulkan counterpart to @c GetGlTextureName: a plugin that rendered into
   * a @c VkImage on the backend's own device (obtained via
   * @c Backend::GetVulkanContext) returns that handle here so a Vulkan
   * compositor can composite it directly — same device, no cross-API/-device
   * import. Returned as @c void* so this header stays free of a Vulkan include;
   * the caller casts back to @c VkImage. Fills @p width / @p height with the
   * image extent. Returns nullptr (default) when the plugin has no Vulkan image
   * (GL path, or nothing rendered yet).
   *
   * Contract: the image is B8G8R8A8_UNORM, its memory holds a complete frame
   * (the plugin has made its writes visible), and it is safe to transition to
   * a transfer-source layout and read this frame.
   */
  [[nodiscard]] virtual void* GetVulkanImage(int32_t* /*width*/,
                                             int32_t* /*height*/) const {
    return nullptr;
  }

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
};
