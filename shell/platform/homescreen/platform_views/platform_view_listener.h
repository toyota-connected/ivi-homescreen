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
  /// When a touch sequence is happening on the embedded UIView all touch events are delayed.
  /// Calling this method releases the delayed events to the embedded UIView and makes it consume
  /// any following touch events for the pointers involved in the active gesture.
  void (*accept_gesture)(int32_t id);
  /// When a touch sequence is happening on the embedded UIView all touch events are delayed.
  /// Calling this method drops the buffered touch events and prevents any future touch events for
  /// the pointers that are part of the active touch sequence from arriving to the embedded view.
  void (*reject_gesture)(int32_t id);
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