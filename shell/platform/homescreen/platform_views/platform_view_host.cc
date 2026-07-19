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

#include "platform_view_host.h"

#include "config/common.h"  // BUILD_COMPOSITOR
#include "flutter_desktop_engine_state.h"
#include "logging/logging.h"

#include "ihs/platform_view.h"
#include "ihs/platform_view_host.h"

// The host bridges an ihs_pv plugin into the shell's compositor, so it only has
// work to do when the compositor is built. Without it, installing the host
// would register views the compositor can never place — make it a no-op
// instead.
#if BUILD_COMPOSITOR

#include <unistd.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include <vulkan/vulkan.h>

#include "backend/backend.h"
#include "dmabuf_vulkan_import.h"
#include "flutter_desktop_view_controller_state.h"
#include "platform_view.h"
#include "platform_view_listener.h"
#include "platform_view_registry.h"
#include "view/compositor_surface_interface.h"
#include "view/flutter_view.h"

namespace {

// Reaches the active Backend for the engine the host was installed for.
Backend* BackendOf(void* user_data) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state != nullptr && state->view_controller != nullptr &&
      state->view_controller->view != nullptr) {
    return state->view_controller->view->GetBackend();
  }
  return nullptr;
}

// A registry-owned PlatformView that fronts an ihs_pv plugin: it holds the
// plugin's IhsPvCallbacks table and per-view state, trampolines the registry's
// platform_view_listener into that table, and is the ICompositorSurface the
// compositor pulls each frame. Submitted dma-bufs are imported into VkImages
// (cached per ring buffer) that GetVulkanImage hands the compositor; before the
// first submit it reports a null image ("nothing to composite this frame").
class IhsPluginView final : public PlatformView, public ICompositorSurface {
 public:
  explicit IhsPluginView(const PlatformViewRegistry::CreateRequest& request)
      : PlatformView(request.id,
                     request.view_type,
                     request.direction,
                     request.left,
                     request.top,
                     request.width,
                     request.height),
        id_(request.id) {}

  ~IhsPluginView() override;

  IhsPluginView(const IhsPluginView&) = delete;
  IhsPluginView& operator=(const IhsPluginView&) = delete;

  // Plugin callback table + per-view state, filled by the factory.
  IhsPvCallbacks callbacks{};
  void* plugin_user_data{nullptr};

  // Negotiated grant, cached from the host's grant() for the accessors.
  uint32_t granted_kind{IHS_PV_KIND_NONE};
  uint32_t drm_plane_id{0};
  int shm_fd{-1};
  size_t shm_stride{0};

  // Imported dma-bufs keyed by the plugin's ring-buffer id — created once per
  // buffer, reused every submit. `current` points at the buffer the plugin most
  // recently submitted (a stable std::map node, so it survives later inserts).
  // The plugin submits from its own thread while the compositor samples on the
  // raster thread, so `mutex` guards this block.
  mutable std::mutex mutex;
  std::map<uint32_t, DmabufVulkanImporter::ImportedImage> buffers;
  DmabufVulkanImporter::ImportedImage* current{nullptr};
  uint32_t current_layout{VK_IMAGE_LAYOUT_UNDEFINED};

  // ICompositorSurface — a Vulkan producer (no backing store, no GL texture).
  bool OnCreateBackingStore(const FlutterBackingStoreConfig*,
                            FlutterBackingStore*) override {
    return false;
  }
  bool OnCollectBackingStore(const FlutterBackingStore*) override {
    return true;
  }
  bool OnPresent(const FlutterLayer*) override { return true; }
  [[nodiscard]] FlutterPlatformViewIdentifier GetIdentifier() const override {
    return id_;
  }
  void OnResize(int32_t, int32_t) override {}

  // The compositor samples the most recently submitted buffer. Null until the
  // first frame arrives — the compositor treats that as "nothing this frame".
  [[nodiscard]] void* GetVulkanImage(int32_t* width,
                                     int32_t* height) const override {
    const std::lock_guard<std::mutex> lock(mutex);
    if (current == nullptr || current->image == VK_NULL_HANDLE) {
      return nullptr;
    }
    if (width != nullptr) {
      *width = static_cast<int32_t>(current->width);
    }
    if (height != nullptr) {
      *height = static_cast<int32_t>(current->height);
    }
    return reinterpret_cast<void*>(current->image);
  }
  [[nodiscard]] uint32_t GetVulkanImageLayout() const override {
    const std::lock_guard<std::mutex> lock(mutex);
    return current_layout;
  }
  void SetVulkanImageLayout(uint32_t layout) override {
    const std::lock_guard<std::mutex> lock(mutex);
    current_layout = layout;
  }

 private:
  int32_t id_;
};

// One importer per process (a single Vulkan device), initialized once from the
// backend's Vulkan context at host install. Stays not-ready when the active
// backend is not Vulkan or lacks the dma-buf import extensions, in which case
// ihs_pv views fall back to producing no frame.
DmabufVulkanImporter g_importer;

IhsPluginView::~IhsPluginView() {
  // NOTE: the compositor may still hold in-flight blits of these images;
  // deferring the destroy behind the release fence is the explicit-sync
  // increment. Today dispose runs after the last present for the view.
  const std::lock_guard<std::mutex> lock(mutex);
  for (auto& [buffer_id, image] : buffers) {
    g_importer.Destroy(&image);
  }
}

// Close every plane fd a frame still owns (its import did not consume them).
void CloseFrameFds(const IhsFrame* frame) {
  for (uint32_t i = 0; i < frame->plane_count && i < 4; ++i) {
    if (frame->plane_fd[i] >= 0) {
      close(frame->plane_fd[i]);
    }
  }
}

// platform_view_listener trampolines: the registry drives these with the
// listener context (the IhsPluginView*), which we forward into the plugin's
// IhsPvCallbacks. accept_gesture/reject_gesture take a bare id (no context), so
// they cannot reach the view here and are left null — gesture-arena arbitration
// for ihs_pv views is a follow-up.
void ListenerResize(double width, double height, void* data) {
  auto* view = static_cast<IhsPluginView*>(data);
  if (view->callbacks.resize != nullptr) {
    view->callbacks.resize(view->plugin_user_data, width, height);
  }
}

void ListenerOnTouch(int32_t action,
                     int32_t point_count,
                     size_t pointer_data_size,
                     const double* pointer_data,
                     void* data) {
  auto* view = static_cast<IhsPluginView*>(data);
  if (view->callbacks.on_touch != nullptr) {
    view->callbacks.on_touch(view->plugin_user_data, action, point_count,
                             pointer_data_size, pointer_data);
  }
}

void ListenerDispose(bool /*hybrid*/, void* data) {
  auto* view = static_cast<IhsPluginView*>(data);
  if (view->callbacks.dispose != nullptr) {
    view->callbacks.dispose(view->plugin_user_data);
  }
}

const platform_view_listener kListener = {
    /* resize */ ListenerResize,
    /* set_direction */ nullptr,
    /* set_offset */ nullptr,
    /* on_touch */ ListenerOnTouch,
    /* dispose */ ListenerDispose,
    /* accept_gesture */ nullptr,
    /* reject_gesture */ nullptr,
};

// --- IhsPvHost implementation -----------------------------------------------

int HostRegisterFactory(void* user_data,
                        const char* view_type,
                        IhsPvFactory factory,
                        void* factory_user_data) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state == nullptr || state->platform_view_registry == nullptr) {
    return IHS_PV_ERR_NO_REGISTRY;
  }
  const std::string type(view_type);
  state->platform_view_registry->RegisterFactory(
      type,
      [factory, factory_user_data, state](
          PlatformViewRegistry& registry,
          const PlatformViewRegistry::CreateRequest& request)
          -> std::unique_ptr<PlatformView> {
        auto view = std::make_unique<IhsPluginView>(request);

        IhsPvCreateInfo info{};
        info.struct_size = sizeof(info);
        info.id = request.id;
        info.view_type = request.view_type.c_str();
        info.direction = request.direction;
        info.left = request.left;
        info.top = request.top;
        info.width = request.width;
        info.height = request.height;
        if (request.params != nullptr && !request.params->empty()) {
          info.params = request.params->data();
          info.params_size = request.params->size();
        }

        const int rc = factory(&info, factory_user_data,
                               reinterpret_cast<IhsPlatformView*>(view.get()),
                               &view->callbacks, &view->plugin_user_data);
        if (rc != IHS_PV_OK) {
          ihs::log::warn("[ihs_pv] factory for '{}' refused (rc={})",
                         request.view_type, rc);
          return nullptr;
        }

        // Drive lifecycle through the registry's listener table, and register
        // the compositor surface so the backend pulls frames. The surface's
        // lifetime is the registry-owned instance; the compositor holds a
        // non-owning alias dropped on UnregisterCompositorSurface at dispose.
        registry.RegisterListener(request.id, &kListener, view.get());
        if (state->view_controller != nullptr &&
            state->view_controller->view != nullptr) {
          state->view_controller->view->RegisterCompositorSurface(
              request.id, std::shared_ptr<ICompositorSurface>(
                              view.get(), [](ICompositorSurface*) {}));
        }
        return view;
      });
  return IHS_PV_OK;
}

void HostUnregisterFactory(void* user_data, const char* view_type) {
  auto* state = static_cast<FlutterDesktopEngineState*>(user_data);
  if (state != nullptr && state->platform_view_registry != nullptr) {
    state->platform_view_registry->UnregisterFactory(std::string(view_type));
  }
}

int HostQueryCapabilities(void* user_data, IhsPvCapabilities* out) {
  out->backend_key = "";
  out->kinds =
      IHS_PV_KIND_SOFTWARE_SHM;  // the universal floor is always offered
  Backend* backend = BackendOf(user_data);
  if (backend != nullptr) {
    BackendVulkanContext vk{};
    if (backend->GetVulkanContext(&vk)) {
      out->kinds |= IHS_PV_KIND_TEXTURE_DMABUF_IMPORT;
    }
  }
  return IHS_PV_OK;
}

int HostVulkanContext(void* user_data, IhsVulkanContext* out) {
  Backend* backend = BackendOf(user_data);
  BackendVulkanContext vk{};
  if (backend == nullptr || !backend->GetVulkanContext(&vk)) {
    return IHS_PV_ERR_UNSUPPORTED;
  }
  out->instance = vk.instance;
  out->physical_device = vk.physical_device;
  out->device = vk.device;
  out->queue = vk.queue;
  out->queue_family_index = vk.queue_family_index;
  out->get_instance_proc_addr = vk.get_instance_proc_addr;
  // device/instance extensions, queue lock, and the VMA allocator are not
  // exposed by the backend yet; the plugin allocates manually until then.
  return IHS_PV_OK;
}

int HostEglContext(void* user_data, IhsEglContext* out) {
  Backend* backend = BackendOf(user_data);
  BackendEglContext egl{};
  if (backend == nullptr || !backend->GetEglContext(&egl)) {
    return IHS_PV_ERR_UNSUPPORTED;
  }
  out->egl_display = egl.display;
  out->egl_context = egl.share_context;
  out->egl_config = egl.config;
  out->gbm_device = nullptr;
  return IHS_PV_OK;
}

int HostGrant(void* /*user_data*/,
              IhsPlatformView* view,
              uint32_t kind,
              const IhsFormatModifier* /*format*/,
              uint32_t* /*out_drm_plane_id*/,
              int* /*out_shm_fd*/,
              size_t* /*out_shm_stride*/) {
  // Record the granted kind on the view. Reserving a DRM plane / shm buffer for
  // the non-Vulkan kinds is wired with the submit path; the Vulkan
  // texture-import kind needs no pull-side reservation.
  reinterpret_cast<IhsPluginView*>(view)->granted_kind = kind;
  return IHS_PV_OK;
}

void HostRevoke(void* /*user_data*/, IhsPlatformView* view) {
  reinterpret_cast<IhsPluginView*>(view)->granted_kind = IHS_PV_KIND_NONE;
}

uint32_t HostGrantDrmPlaneId(void* /*user_data*/, IhsPlatformView* view) {
  return reinterpret_cast<IhsPluginView*>(view)->drm_plane_id;
}

int HostGrantShmFd(void* /*user_data*/,
                   IhsPlatformView* view,
                   size_t* out_stride) {
  auto* v = reinterpret_cast<IhsPluginView*>(view);
  if (out_stride != nullptr) {
    *out_stride = v->shm_stride;
  }
  return v->shm_fd;
}

int HostSubmit(void* user_data,
               IhsPlatformView* view,
               const IhsFrame* frame,
               int acquire_fence_fd,
               int* out_release_fence_fd) {
  if (out_release_fence_fd != nullptr) {
    *out_release_fence_fd = -1;
  }
  // Explicit sync is a follow-up; consume the acquire fence so it does not
  // leak.
  if (acquire_fence_fd >= 0) {
    close(acquire_fence_fd);
  }

  (void)user_data;
  auto* v = reinterpret_cast<IhsPluginView*>(view);
  if (!g_importer.ready()) {
    CloseFrameFds(frame);
    return IHS_PV_ERR_NO_BACKEND;
  }

  const std::lock_guard<std::mutex> lock(v->mutex);
  if (auto it = v->buffers.find(frame->buffer_id); it != v->buffers.end()) {
    // Known ring buffer: the submitted fd is a redundant handle to the same
    // memory. Close it and reuse the existing import.
    CloseFrameFds(frame);
    v->current = &it->second;
  } else {
    DmabufVulkanImporter::ImportedImage imported;
    if (!g_importer.Import(*frame, &imported)) {
      CloseFrameFds(frame);  // import left the fds untouched on failure
      return IHS_PV_ERR_INVALID;
    }
    // Import consumed plane_fd[0]; a single-plane RGB frame owns no other fds.
    auto [pos, inserted] = v->buffers.emplace(frame->buffer_id, imported);
    v->current = &pos->second;
  }

  // The plugin re-rendered the buffer before submitting, so the compositor
  // transitions from GENERAL to read it. A spec-correct foreign-queue-family
  // acquire from the producer is the explicit-sync increment.
  v->current_layout = VK_IMAGE_LAYOUT_GENERAL;
  return IHS_PV_OK;
}

// Process-global host; user_data re-points at the most recently installed
// engine (single-engine today).
IhsPvHost g_host{};

}  // namespace

void InstallPlatformViewHost(FlutterDesktopEngineState* engine_state) {
  if (engine_state == nullptr ||
      engine_state->platform_view_registry == nullptr) {
    return;
  }
  g_host.struct_size = sizeof(g_host);
  g_host.user_data = engine_state;
  g_host.register_factory = HostRegisterFactory;
  g_host.unregister_factory = HostUnregisterFactory;
  g_host.query_capabilities = HostQueryCapabilities;
  g_host.vulkan_context = HostVulkanContext;
  g_host.egl_context = HostEglContext;
  g_host.grant = HostGrant;
  g_host.revoke = HostRevoke;
  g_host.grant_drm_plane_id = HostGrantDrmPlaneId;
  g_host.grant_shm_fd = HostGrantShmFd;
  g_host.submit = HostSubmit;
  ihs_pv_set_host(&g_host);

  // Bring up the dma-buf importer once, on this thread, from the backend's
  // Vulkan context — so the submit path (which runs off-thread) never races an
  // init. Not-ready is fine: the view simply produces no frame.
  if (Backend* backend = BackendOf(engine_state); backend != nullptr) {
    if (BackendVulkanContext vk{}; backend->GetVulkanContext(&vk)) {
      g_importer.Init(static_cast<VkInstance>(vk.instance),
                      static_cast<VkPhysicalDevice>(vk.physical_device),
                      static_cast<VkDevice>(vk.device),
                      vk.get_instance_proc_addr);
    }
  }
  ihs::log::debug("[ihs_pv] platform-view host installed (dma-buf import {})",
                  g_importer.ready() ? "ready" : "unavailable");
}

#else  // !BUILD_COMPOSITOR

void InstallPlatformViewHost(FlutterDesktopEngineState* /*engine_state*/) {}

#endif  // BUILD_COMPOSITOR
