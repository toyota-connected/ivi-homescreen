## Platform View Plugins

When `-DBUILD_COMPOSITOR=ON`, the EGL and Vulkan backends wire the `FlutterCompositor` backing-store API so that a plugin-owned native surface can be interleaved between Flutter-rendered layers — what the Flutter framework calls a `PlatformViewLayer`. Without this flag the engine runs in single-surface mode and platform views fall back to full-screen overlays.

A plugin participates by implementing `ICompositorSurface` (`shell/view/compositor_surface_interface.h`) and registering the instance with `FlutterView` when the Dart widget is created:

```cpp
#include "view/compositor_surface_interface.h"
#include "view/flutter_view.h"

class MyPlatformView : public ICompositorSurface {
 public:
  explicit MyPlatformView(FlutterPlatformViewIdentifier id) : id_(id) {}

  bool OnCreateBackingStore(const FlutterBackingStoreConfig* config,
                            FlutterBackingStore* out) override {
    // Fill `out` with a backing store your plugin renders into. Most
    // plugins delegate to the engine-provided backing store and only
    // override if they need a specific image/format (e.g. DMA-BUF).
    return false;
  }
  bool OnCollectBackingStore(const FlutterBackingStore*) override { return true; }
  bool OnPresent(const FlutterLayer* layer) override {
    // Draw / swap your native surface here. The compositor has already
    // reconciled the Wayland subsurface Z-order before this call.
    return true;
  }
  FlutterPlatformViewIdentifier GetIdentifier() const override { return id_; }
  void OnResize(int32_t w, int32_t h) override { /* re-size native surface */ }

 private:
  FlutterPlatformViewIdentifier id_;
};

// On the flutter/platform_views "create" message:
flutter_view->RegisterCompositorSurface(
    id, std::make_shared<MyPlatformView>(id));

// On "dispose":
flutter_view->UnregisterCompositorSurface(id);
```

`PlatformViewsHandler` calls `UnregisterCompositorSurface` automatically on `dispose` and routes `resize` messages to `ICompositorSurface::OnResize`, so plugins only need to register on create.

Key details:

- `OnPresent` runs on the engine's rasterizer thread. If your plugin renders on its own thread, marshal work there.
- The Vulkan backend hands the engine a `VkImage` in `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`. The compositor records all layout transitions — plugins that *consume* the image via `ICompositorSurface::OnPresent` should be prepared to sample in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` (compositor transitions it to `TRANSFER_SRC_OPTIMAL` during the blit; see `WaylandVulkanBackend::BlitStoreToSwapchain`).
- With `-DBUILD_COMPOSITOR_DMABUF_EXPORT=ON`, a plugin can query `WaylandVulkanBackend::HasDmaBufExport()` to know whether the store exposes a DMA-BUF fd for zero-copy import into EGL/KMS/Filament.
- On pure GLES2 drivers, the EGL compositor falls back to a textured-quad program instead of `glBlitFramebuffer`; no plugin action required.

Compositor mode is opt-in. The default (`BUILD_COMPOSITOR=OFF`) build is unchanged, so existing plugins that draw via a legacy `wl_subsurface` path keep working as before.

