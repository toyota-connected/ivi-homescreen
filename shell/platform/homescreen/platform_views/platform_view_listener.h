#ifndef FLUTTER_PLUGIN_PLATFORM_VIEW_INTERFACE_H_
#define FLUTTER_PLUGIN_PLATFORM_VIEW_INTERFACE_H_

#if defined(__cplusplus)
extern "C" {
#endif

struct platform_view_listener {
  void (*resize)(double width, double height, void* data);
  void (*set_direction)(int32_t direction, void* data);
  void (*set_offset)(double left, double top, void* data);
  void (*on_touch)(int32_t action, double x, double y, void* data);
  void (*dispose)(bool hybrid, void* data);
};

typedef void (*PlatformViewAddListener)(
    void* context,
    int32_t id,
    const struct platform_view_listener* listener,
    void* listener_context);

typedef void (*PlatformViewRemoveListener)(void* context, int32_t id);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // FLUTTER_PLUGIN_PLATFORM_VIEW_INTERFACE_H_