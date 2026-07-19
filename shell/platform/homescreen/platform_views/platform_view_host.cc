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

#include <cstdint>
#include <memory>
#include <string>

#include "backend/backend.h"
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
// compositor pulls each frame. The submit path that feeds GetVulkanImage a real
// VkImage is wired in a follow-up; today it registers cleanly and routes
// lifecycle, and the compositor sees "no frame yet" (null image).
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

  // No frame is available until the submit path is wired; the compositor treats
  // a null image as "nothing to composite this frame".
  [[nodiscard]] void* GetVulkanImage(int32_t*, int32_t*) const override {
    return nullptr;
  }

 private:
  int32_t id_;
};

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

int HostSubmit(void* /*user_data*/,
               IhsPlatformView* /*view*/,
               const IhsFrame* /*frame*/,
               int /*acquire_fence_fd*/,
               int* out_release_fence_fd) {
  // Importing the submitted dma-buf into a VkImage the compositor samples is
  // the next increment; until then a submit is accepted but produces no frame.
  if (out_release_fence_fd != nullptr) {
    *out_release_fence_fd = -1;
  }
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
  ihs::log::debug("[ihs_pv] platform-view host installed");
}

#else  // !BUILD_COMPOSITOR

void InstallPlatformViewHost(FlutterDesktopEngineState* /*engine_state*/) {}

#endif  // BUILD_COMPOSITOR
