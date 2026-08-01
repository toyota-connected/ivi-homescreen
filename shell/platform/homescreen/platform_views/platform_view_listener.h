/*
 * Copyright 2020-2023 Toyota Connected North America
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

#ifndef FLUTTER_PLUGIN_PLATFORM_VIEW_INTERFACE_H_
#define FLUTTER_PLUGIN_PLATFORM_VIEW_INTERFACE_H_

#if defined(__cplusplus)
extern "C" {
#endif

struct platform_view_listener {
  void (*resize)(double width, double height, void* data);
  /// Sets the layout direction for the Android view.
  void (*set_direction)(int32_t direction, void* data);
  void (*set_offset)(double left, double top, void* data);
  void (*on_touch)(int32_t action,
                   int32_t point_count,
                   const size_t pointer_data_size,
                   const double* pointer_data,
                   void* data);
  void (*dispose)(bool hybrid, void* data);
  /// Gesture-arena arbitration for a sequence already delivered to the view:
  /// accept releases the delayed touches to it and lets it consume the rest of
  /// the sequence, reject drops them and sends it nothing further for those
  /// pointers.
  ///
  /// These are an iOS concept ("the embedded UIView" of the original docs) and
  /// nothing on Linux drives them today: Flutter sends acceptGesture /
  /// rejectGesture from DarwinPlatformViewController alone, while the
  /// PlatformViewSurface path used here settles the arena inside the render
  /// object without a platform message. They take @p data like every other
  /// entry so a host can reach the view they concern, rather than being the
  /// two a host cannot implement.
  void (*accept_gesture)(int32_t id, void* data);
  void (*reject_gesture)(int32_t id, void* data);
  /// The grant this view was given is no longer valid -- its output changed
  /// mode, went away, or the plane backing it was taken -- so whatever the
  /// view negotiated has to be negotiated again or dropped to the floor.
  ///
  /// Delivered per view rather than broadcast: only the views bound to the
  /// output that changed are told, so a second display coming and going does
  /// not disturb the first.
  void (*renegotiate)(int32_t id, void* data);
};

typedef void (*PlatformViewAddListener)(void* context,
                                        int32_t id,
                                        const platform_view_listener* listener,
                                        void* listener_context);

typedef void (*PlatformViewRemoveListener)(void* context, int32_t id);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // FLUTTER_PLUGIN_PLATFORM_VIEW_INTERFACE_H_