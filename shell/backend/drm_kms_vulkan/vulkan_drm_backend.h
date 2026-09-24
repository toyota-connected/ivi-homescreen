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

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vulkan.h>

#include "backend/backend.h"
// drm_config::TriState — the yes/no/auto knobs both DRM backends share. The
// EGL backend's header is where they are declared; only the enum is used here.
#include "backend/drm_kms_egl/drm_backend.h"
#include "backend/drm_kms_vulkan/device_caps.h"
#if BUILD_COMPOSITOR
// Pure-Vulkan blend pipeline; despite living beside the Wayland backend it
// pulls in no Wayland headers, so the DRM backend shares it rather than
// carrying a second copy of the same render pass.
#include "backend/wayland_vulkan/vulkan_queue_interposer.h"
#include "backend/wayland_vulkan/wl_layer_compositor.h"
#include "view/compositor_surface_interface.h"
#include "view/layer_scanout.h"
#endif
#include "profiling/frame_profile.h"
#include "vsync/ivsync_provider.h"

namespace homescreen {
class DrmSession;
class DrmCursor;
}  // namespace homescreen

namespace ihs::hud {
class VulkanHud;
}  // namespace ihs::hud

// DRM/KMS scanout backend that drives the Flutter Vulkan renderer and presents
// on hardware KMS planes via zero-copy dma-buf import, reusing the session /
// modeset / vsync stack from drm_kms_egl below the pixel layer.
//
// Create() brings up the Vulkan instance, selects a physical device that can do
// zero-copy dma-buf scanout (render-node-aware when the device exposes a DRM
// node, vendor/name fallback otherwise), creates the logical device and queues,
// then opens the DRM device, takes master, discovers the scanout target, and
// builds the LayerScene the present path commits onto. The Flutter compositor
// callbacks back each backing store with an exported modifier VkImage and scan
// it out zero-copy on the primary plane.
class VulkanDrmBackend final : public Backend {
 public:
  // Bring up the device and the present path. Returns nullptr on failure: the
  // caller treats null as a hard init failure and aborts, exactly as the
  // drm_kms_egl backend does on DrmBackend::Create returning null. @p session
  // may be null when no libseat session is available. @p mode_spec selects the
  // scanout mode ("<W>x<H>[@<R>]"); empty uses the connector's preferred mode.
  //
  // @p explicit_sync forces the scanout hand-off: kYes fails init when the
  // device cannot export a SYNC_FD semaphore rather than degrading silently,
  // kNo takes the CPU-fence path, kAuto uses explicit sync when available.
  // @p connector_name pins the panel (--drm-connector); empty picks the first
  // connected connector, which is a coin flip on a card that also exposes a
  // virtual connector.
  static std::shared_ptr<VulkanDrmBackend> Create(
      const std::string& drm_device,
      bool enable_validation,
      homescreen::DrmSession* session,
      const std::string& mode_spec,
      const std::string& connector_name,
      int rotation,
      drm_config::TriState explicit_sync = drm_config::TriState::kAuto);

  // wayland-leased-drm: drive an externally-owned DRM fd (the master fd from
  // wp_drm_lease_v1.lease_fd) instead of opening a card by path.
  //
  // The lease only changes how the fd was obtained; the Vulkan side is
  // unaffected. Unlike the EGL path, the VkDevice was never created from the
  // KMS fd — the physical device is matched by DRM dev-number and lives on the
  // ICD's render node — so only the scanout/modeset half touches the lease.
  //
  // @p drm_fd is borrowed: this backend never closes it. @p fd_owner is the
  // opaque keep-alive that does own it (the LeaseHold) and must outlive the
  // backend. @p drm_device is still needed, derived from the lease fd, because
  // the physical-device match stats the node — it is never opened on this path.
  //
  // @p connector_id is the leased connector to drive; a lease may hold more
  // than one (the compositor picks the final object set), so without it the
  // scanout probe takes "first connected" and can drive the wrong panel. 0 =
  // that heuristic.
  //
  // @p revoked is polled before each atomic commit: a revoked lease's KMS
  // objects are gone from the fd's view, so every commit naming them fails.
  // Null = never gated.
  static std::shared_ptr<VulkanDrmBackend> Create(
      int drm_fd,
      std::shared_ptr<void> fd_owner,
      const std::string& drm_device,
      bool enable_validation,
      const std::string& mode_spec,
      int rotation,
      uint32_t connector_id,
      std::function<bool()> revoked,
      drm_config::TriState explicit_sync = drm_config::TriState::kAuto);

  ~VulkanDrmBackend() override;

  VulkanDrmBackend(const VulkanDrmBackend&) = delete;
  VulkanDrmBackend& operator=(const VulkanDrmBackend&) = delete;

  // ── Backend interface ──────────────────────────────────────────────────────
  // Present runs through the Flutter compositor callbacks
  // (GetCompositorConfig), so these size/surface/texture entry points are no-op
  // stubs that satisfy the vtable and the FlutterView call sites.
  void Resize(size_t index,
              Engine* flutter_engine,
              int32_t width,
              int32_t height) override;
  void CreateSurface(size_t index,
                     struct wl_surface* surface,
                     int32_t width,
                     int32_t height) override;
  bool TextureMakeCurrent() override;
  bool TextureClearCurrent() override;
  FlutterRendererConfig GetRenderConfig() override;
  FlutterCompositor GetCompositorConfig() override;

  /// --drm-compositor for this backend. kPlanes selects the compositor even
  /// under Impeller, which needs an engine that renders Impeller into Vulkan
  /// backing stores; kGl selects the root surface; kAuto uses the compositor
  /// except under Impeller. Set before the engine starts.
  void SetCompositorMode(const drm_config::Compositor mode) {
    compositor_mode_ = mode;
  }

  // Expose the backend's Vulkan device to the ihs_pv platform-view host so a
  // plugin (e.g. the Mapbox Maps SDK) can adopt the shared device, render into
  // an exported dma-buf, and submit it for zero-copy import — the same seam
  // WaylandVulkanBackend provides.
  bool GetVulkanContext(BackendVulkanContext* out) const override;

#if BUILD_COMPOSITOR
  // Platform-view surfaces. Backend's default implementations are no-ops, so
  // without these every surface the registry hands over is dropped and the
  // views composite as nothing.
  void RegisterCompositorSurface(
      FlutterPlatformViewIdentifier id,
      std::shared_ptr<ICompositorSurface> surface) override;
  void UnregisterCompositorSurface(FlutterPlatformViewIdentifier id) override;
  void ResizeCompositorSurface(FlutterPlatformViewIdentifier id,
                               int32_t width,
                               int32_t height) override;
#endif

  // Vsync: drive Flutter's frame scheduling from the real page-flip event
  // instead of its wall-clock fallback, so frames align to the connector's
  // refresh (and the raster thread no longer blocks in WaitForFlip). Returns a
  // trampoline that parks the baton in vsync_; the async flip reader returns it
  // via DeliverVsync. IVI_DRMVK_VSYNC=0 disables it (wall-clock fallback).
  [[nodiscard]] VsyncCallback GetVsyncCallback() const override;
  void SetEngineHandle(FLUTTER_API_SYMBOL(FlutterEngine) engine) override {
    engine_handle_.store(engine, std::memory_order_release);
    vsync_.SetEngine(engine,
                     platform_task_runner_.load(std::memory_order_acquire));
  }
  void SetPlatformTaskRunner(TaskRunner* runner) override {
    platform_task_runner_.store(runner, std::memory_order_release);
    vsync_.SetEngine(engine_handle_.load(std::memory_order_acquire), runner);
  }
  void SetVsyncParked(const bool parked) override { vsync_.SetParked(parked); }

  void StopVsyncMonitor() override;

  [[nodiscard]] uint32_t width() const { return width_; }
  [[nodiscard]] uint32_t height() const { return height_; }
  // Scanout rotation in degrees (0|90|180|270). FlutterView forwards it to the
  // seat so the HW cursor sprite is transformed from render space (where the
  // pointer lives) into panel space (where the cursor plane lives).
  [[nodiscard]] int rotation() const { return rotation_; }
  [[nodiscard]] const drm_kms_vulkan::DeviceCaps& caps() const { return caps_; }

  // The self-committing DRM HW cursor (null when xcursor/drm-cxx-cursor is
  // unavailable or --disable-cursor). FlutterView forwards it to the seat via
  // DrmDisplay::SetCursor so pointer motion drives the on-screen sprite.
  [[nodiscard]] homescreen::DrmCursor* drm_cursor() const {
    return cursor_.get();
  }

 private:
  VulkanDrmBackend(std::string drm_device,
                   bool enable_validation,
                   homescreen::DrmSession* session,
                   std::string mode_spec,
                   std::string connector_name,
                   int rotation);

  // Shared tail of both Create() overloads: bring-up + compositor setup, which
  // are identical once the backend knows where its DRM fd comes from.
  static std::shared_ptr<VulkanDrmBackend> FinishCreate(
      std::shared_ptr<VulkanDrmBackend> backend);

  // Bring-up steps. Each logs and returns false on failure; refusal_reason
  // carries the cause for gate failures.
  bool BringUp(std::string& refusal_reason);
  bool CreateInstance(std::string& refusal_reason);
  void SetupDebugMessenger();
  bool SelectPhysicalDevice(std::string& refusal_reason);
  bool CreateLogicalDevice(std::string& refusal_reason);
  void PopulateCaps();
  void Teardown();

  // Open the DRM device, take master, build the LayerScene for the discovered
  // scanout target, and cache the negotiated modifier set. On success the
  // backend can present and Create() returns it instead of refusing.
  bool SetupCompositor(std::string& err);

  // Root-surface renderer callbacks. The compositor path renders into backing
  // stores and presents via present_layers, so these are never invoked at
  // runtime, but the embedder rejects a Vulkan renderer config that leaves them
  // null.
  // Pick or grow a free scanout slot of this size, importing its framebuffer
  // into the ring. Returns the slot index, or -1 on failure.
  int AcquireScanoutSlot(uint32_t width, uint32_t height);

  // Hand an acquired slot back to the engine as a FlutterBackingStore.
  bool FinishBackingStore(int slot, FlutterBackingStore* out);

  // Shared tail of both present paths; see the definition.
  bool PresentSlot(size_t slot,
                   const FlutterLayer** layers,
                   size_t count,
                   uint64_t t0);

  static FlutterVulkanImage GetNextImageCb(void* user_data,
                                           const FlutterFrameInfo* frame_info);
  static bool PresentImageCb(void* user_data, const FlutterVulkanImage* image);

  // Flutter compositor callbacks (user_data == this) and their implementations.
  static bool CreateBackingStoreCb(const FlutterBackingStoreConfig* config,
                                   FlutterBackingStore* out,
                                   void* user_data);
  static bool CollectBackingStoreCb(const FlutterBackingStore* store,
                                    void* user_data);
  static bool PresentLayersCb(const FlutterLayer** layers,
                              size_t count,
                              void* user_data);
  // Called as stores are handed out. Reports once if the engine has taken
  // several and presented none, which is what an engine without Impeller
  // Vulkan backing stores looks like from here.
  void ReportIfEngineNeverPresents();

  bool CreateBackingStoreImpl(const FlutterBackingStoreConfig* config,
                              FlutterBackingStore* out);
  bool CollectBackingStoreImpl(const FlutterBackingStore* store);
  bool PresentLayersImpl(const FlutterLayer** layers, size_t count);

  std::string drm_device_;

  // wayland-leased-drm: a borrowed DRM fd to use instead of opening
  // drm_device_. -1 on the path-opened path. When set, drm_device_ is only ever
  // stat()ed (physical-device match), never opened -- see the leased Create().
  int injected_fd_ = -1;
  // Keeps the injected fd's owner (the LeaseHold) alive for this backend's
  // lifetime. Opaque so this header stays free of the lease client: the backend
  // needs the fd to stay valid, not to know what keeps it so.
  std::shared_ptr<void> fd_owner_;

  // The leased connector to drive (0 = first connected with a mode) and the
  // lease's revocation gate (empty on the path-opened path, where there is no
  // lease to lose). Both inert unless injected_fd_ >= 0.
  uint32_t lease_connector_id_ = 0;
  std::function<bool()> lease_revoked_;
  // One-shot latch so the revocation notice is logged once, not once per frame.
  std::atomic<bool> lease_revoked_logged_{false};

  // Scanout mode selector ("<W>x<H>[@<R>]"); empty = connector preferred mode.
  std::string mode_spec_;
  // --drm-connector / view.backend.drm.connector. Empty = first connected
  // connector with a mode. Unused on the leased tier, which pins by id.
  std::string connector_name_;
  // DRM scanout rotation in degrees (0|90|180|270). 90/270 swap the render /
  // viewport extent against the CRTC mode; lowered to the plane rotation
  // property at present time.
  int rotation_ = 0;
  // --drm-explicit-sync. Read once in SetupCompositor: kNo forces the CPU-fence
  // scanout path, kYes refuses to start when the device cannot export a SYNC_FD
  // semaphore, kAuto uses explicit sync when it is available.
  drm_config::TriState explicit_sync_pref_ = drm_config::TriState::kAuto;
  // --drm-compositor. Decides between the compositor (backing stores, one KMS
  // plane per layer) and the root surface; see GetCompositorConfig.
  drm_config::Compositor compositor_mode_ = drm_config::Compositor::kAuto;
  // Backing stores handed to the engine, and whether any of them ever came
  // back through present_layers. An engine that cannot render Impeller into a
  // Vulkan backing store takes every store and presents none; see
  // ReportIfEngineNeverPresents.
  uint32_t backing_stores_created_ = 0;
  bool layers_presented_ = false;
  bool never_presents_reported_ = false;
  // Unified cadence profiler (IVI_PROFILE / legacy IVI_DRMVK_PROFILE). Written
  // from the rasterizer thread (PresentLayersImpl) only.
  profiling::FrameProfile frame_profile_;
  bool enable_validation_ = false;
  // Reused by the session/vsync glue in a later change; held now so Create()'s
  // signature and the FlutterView wiring are stable.
  [[maybe_unused]] homescreen::DrmSession* session_ = nullptr;  // not owned

  uint32_t width_ = 0;
  uint32_t height_ = 0;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT debug_messenger_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t graphics_queue_family_ = UINT32_MAX;
  VkQueue graphics_queue_ = VK_NULL_HANDLE;
  // Guards every host access to graphics_queue_. The engine, this backend and
  // a platform-view plugin all submit to it; Vulkan requires host access to a
  // VkQueue to be externally synchronized. Registered with QueueInterposer so
  // the trampolines handed to the engine and to plugins take *this* mutex --
  // a second lock over the same queue would serialize nothing.
  std::mutex queue_mutex_;

  // Give each Flutter layer its own KMS plane rather than blending them all
  // into the bottom one (IVI_DRMVK_PLANE_LAYERS). Resolved once at compositor
  // setup because it decides the scanout format.
  bool plane_layers_ = false;
  // Latched off after a commit failure the plane path cannot be blamed out of,
  // so a bad frame does not become a bad session of retry churn.
  bool plane_layers_latched_off_ = false;
  // Planes the scanout CRTC can drive, cursor included (plane layers only).
  size_t crtc_plane_count_ = 0;
  // Set while frames have more layers than PlaneBudget, so that is said once
  // per run of such frames.
  bool plane_budget_exceeded_ = false;
  // Frames the allocator turned down, and when to ask again: about two
  // seconds at 60 Hz.
  static constexpr uint64_t kPlaneRetryPresents = 120;
  PlanePathBackoff plane_backoff_{kPlaneRetryPresents};
  // The primary plane's own zpos; overlays are placed above it, not above 0.
  int primary_zpos_ = 0;
  // Said once: a frame shape whose layers the allocator would not all place.
  bool plane_test_rejected_ = false;
  // Layers on planes as of the last commit, logged when it changes; 0 until
  // the first plane commit. An absence of warnings is not evidence the path
  // ran, so this is the positive signal.
  size_t plane_layers_confirmed_ = 0;

  std::vector<const char*> enabled_instance_extensions_;
  std::vector<const char*> enabled_instance_layers_;
  std::vector<const char*> enabled_device_extensions_;

  drm_kms_vulkan::DeviceCaps caps_;

  // DRM device + LayerScene + backing-store registry. Held behind a pimpl so
  // the drm-cxx scene/device headers stay out of this header (it is included by
  // FlutterView and other GL-free translation units).
  struct CompositorState;
  std::unique_ptr<CompositorState> compositor_;
  // The renderer-config callbacks (get_next_image / present_image) receive the
  // engine state as user_data, not the backend, and there is no route from one
  // to the other. This backend holds DRM master, so there is at most one, and
  // those callbacks reach it through here.
  static std::atomic<VulkanDrmBackend*> s_active_;

  // Record + submit the scanout hand-off barrier for @p image. On the
  // explicit-sync path returns an owned sync_file fd (>=0) the caller wraps as
  // the ready slot's acquire fence (KMS waits via IN_FENCE_FD); on the
  // CPU-fence fallback the barrier is waited on here and -1 is returned.
  int SubmitScanoutBarrier(CompositorState& c,
                           VkImage image,
                           VkImageView view,
                           uint32_t width,
                           uint32_t height,
                           const FlutterLayer** layers,
                           size_t count,
                           const std::vector<VkImage>* plane_images = nullptr);

  // Submit ring entry @p i's already-ended command buffer and export its
  // signal semaphore as a sync_file. Shared by the blend and plane paths --
  // they differ in what they record, not in how it reaches KMS.
  int SubmitSyncRing(CompositorState& c, size_t i);

#if BUILD_HUD
  // Debug HUD (imgui Vulkan). Lazily created on the first present when IVI_HUD
  // or [hud].enable is set; recorded into the scanout-barrier command buffer so
  // the scanout fence covers it. Explicit-sync path only.
  std::unique_ptr<ihs::hud::VulkanHud> hud_;
  bool hud_enabled_{false};
  bool hud_checked_{false};
  bool hud_init_failed_{false};
  uint64_t hud_last_present_ns_{0};
  std::array<float, 60> hud_interval_ms_{};
  uint32_t hud_interval_head_{0};
  uint32_t hud_interval_count_{0};

  // Ensure hud_ exists and record it into @cmd over the store image (already in
  // GENERAL). @layers/@count feed per-view tracking. Returns true if it drew.
  bool RecordHud(VkCommandBuffer cmd,
                 VkImageView view,
                 uint32_t width,
                 uint32_t height,
                 const FlutterLayer** layers,
                 size_t count);
#endif

#if BUILD_COMPOSITOR
  // Platform-view surfaces, keyed by the engine's view identifier. Registered
  // from the platform thread and read on the raster thread during compositing,
  // hence the mutex.
  std::mutex compositor_surfaces_mu_;
  std::unordered_map<FlutterPlatformViewIdentifier,
                     std::shared_ptr<ICompositorSurface>>
      compositor_surfaces_;

  // src-over blend of the platform views (and any Flutter overlay stores) on
  // top of the base backing store. Built lazily on first use because it is
  // keyed to the slot format, which is not known until the scanout target has
  // been negotiated. kPreserve: the base layer is already in the target.
  std::unique_ptr<wl_vulkan::LayerCompositor> layer_compositor_;
  bool layer_compositor_failed_{false};

  // Blend every layer above the base backing store into @p target_view.
  //
  // Any backing store whose image is @p target_image is the base and is skipped
  // --- it is already resident there, and sampling it would mean reading the
  // image being rendered into. Records into @p cmd and reports whether it
  // opened a render pass, which the caller needs because that pass leaves the
  // target in GENERAL and so replaces the explicit scanout barrier. Raster
  // thread.
  // Import a platform view's acquire fence (a sync_file from an explicit-sync
  // producer) as a semaphore for this frame's submit to wait on. A no-op when
  // the producer stalled instead of handing one over, which is what an
  // implicit-sync producer does. Raster thread.
  void CollectAcquireWait(CompositorState& c,
                          ICompositorSurface* surface,
                          size_t layer = 0);

  // Platform-view surfaces this frame's blend sampled, collected by
  // CompositeOverlays and consumed by PublishReleaseFence once the submit that
  // reads them has an exported sync_file. Raster thread only; cleared at the
  // start of every blend.
  std::vector<std::shared_ptr<ICompositorSurface>> sampled_views_;

  // Hand @p sync_fd -- the scanout submit's exported sync_file -- to every
  // surface that blend sampled, so a producer has something to wait on before
  // it redraws a ring slot. Each surface gets its own dup: SetReleaseFenceFd
  // takes ownership, while @p sync_fd stays the caller's. Raster thread.
  void PublishReleaseFence(int sync_fd);

  bool CompositeOverlays(VkCommandBuffer cmd,
                         const FlutterLayer** layers,
                         size_t count,
                         VkImage target_image,
                         VkImageView target_view,
                         uint32_t width,
                         uint32_t height,
                         uint64_t frame);

  // Add, update or prune one scene layer per backing-store layer, so each can
  // reach its own KMS plane instead of being blended into the bottom one.
  // False means this frame cannot go that way -- a platform view in the
  // stack, an unknown store, or a source/layer that could not be made -- and
  // the caller should blend. Layers added here survive the frame; the caller
  // drops them with DropPlaneLayers when the plan is abandoned.
  bool ReconcilePlaneLayers(CompositorState& c,
                            const FlutterLayer** layers,
                            size_t count);

  // Present this frame with every layer on its own KMS plane: reconcile the
  // scene, ask whether the allocator can place them all, and commit if so.
  // False means it could not, and the caller should present through the blend
  // path instead -- nothing has been committed and any layers added on the way
  // are removed again.
  bool PresentLayersViaPlanes(const FlutterLayer** layers, size_t count);

  // The commit half of the plane path, once the plan is known good. @p
  // assigned is the layer count test() reported, or 0 when the plan was
  // reused and no test ran this frame.
  bool CommitPlaneFrame(CompositorState& c,
                        const FlutterLayer** layers,
                        size_t count,
                        size_t assigned);

  // One platform view within ReconcilePlaneLayers: add or update a scene
  // layer for each of the view's layers and submit each one's newest dma-buf
  // to its pool. Each placed layer takes the next @p z_index. False means the
  // frame cannot take the plane path.
  bool ReconcilePlatformViewLayers(CompositorState& c,
                                   const FlutterLayer& fl,
                                   int& z_index,
                                   std::vector<const void*>& present);

  // Planes the scene can give layers on the scanout CRTC.
  [[nodiscard]] size_t PlaneBudget() const;
  // The planes a frame would take, and a hash of its shape (its layers'
  // kinds, places, platform views and their layer counts). Takes nothing from
  // the producers.
  std::pair<size_t, size_t> FramePlaneDemand(const FlutterLayer** layers,
                                             size_t count);

  // Remove every scene layer ReconcilePlaneLayers added and forget them.
  static void DropPlaneLayers(CompositorState& c);

  // Hand back every platform-view buffer the previous present displaced, with
  // its release fence where the CRTC produced one. Deliberately one present
  // late: drm-cxx releases at displacement, not at flip completion.
  static void DrainDeferredScanoutReleases(CompositorState& c);

  // An OPTIMAL, sampleable image of this size to copy a backing store into,
  // for the case where the store's own modifier cannot be sampled (#617).
  // @p slot indexes within the current frame, which may need several. The
  // image is owned by the compositor's per-frame ring and stays valid until
  // that ring entry comes round again. VK_NULL_HANDLE if it cannot be made,
  // which leaves the caller to sample the store and warn.
  VkImage AcquireSampleScratch(CompositorState& c,
                               size_t slot,
                               uint32_t width,
                               uint32_t height,
                               VkFormat format);
#endif

  // Flutter's vsync_callback -> parks the baton in vsync_. Static C ABI; the
  // engine handle comes from the FlutterDesktopEngineState* user_data.
  static void VsyncTrampoline(void* user_data, intptr_t baton);
  void SetVsyncBaton(FLUTTER_API_SYMBOL(FlutterEngine) engine, intptr_t baton);

  // Arm (and re-arm) the async_wait for the next page-flip event on the reader
  // thread. Runs drmHandleEvent -> OnFlipEvent when the fd is readable.
  void ArmFlipRead();

  // Page-flip completion, on the async flip-reader thread (routed via the
  // commit's user_data). Clears the pending flag and returns the baton with the
  // kernel scanout time; touches no slot state (that stays
  // raster-thread-local).
  void OnFlipEvent(unsigned int tv_sec, unsigned int tv_usec);

  // Backend-spanning vsync baton machinery; the async flip reader feeds it.
  ivi::IVsyncProvider vsync_;
  std::atomic<FLUTTER_API_SYMBOL(FlutterEngine)> engine_handle_{nullptr};
  std::atomic<TaskRunner*> platform_task_runner_{nullptr};

  // Self-committing HW cursor on the scanout CRTC's cursor plane. Created in
  // SetupCompositor against compositor_'s DRM device; destroyed before
  // compositor_ so it never outlives that device.
  std::unique_ptr<homescreen::DrmCursor> cursor_;
};
