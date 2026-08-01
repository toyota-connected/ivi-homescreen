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

#include "platform_view_registry.h"

#include <utility>

// Defines BUILD_COMPOSITOR (generated). Without it the compositor-surface
// safety nets below silently compile out and disposed views leak their surface.
#include "config/common.h"
#include "logging/logging.h"

#if BUILD_COMPOSITOR
#include "flutter_desktop_engine_state.h"
#include "flutter_desktop_view_controller_state.h"
#include "view/flutter_view.h"
#endif

PlatformViewRegistry::PlatformViewRegistry(
    FlutterDesktopEngineState* engine_state)
    : engine_state_(engine_state) {}

void PlatformViewRegistry::RegisterFactory(const std::string& view_type,
                                           Factory factory) {
  std::lock_guard<std::mutex> lock(mutex_);
  factories_[view_type] = std::move(factory);
}

void PlatformViewRegistry::UnregisterFactory(const std::string& view_type) {
  std::lock_guard<std::mutex> lock(mutex_);
  factories_.erase(view_type);
}

bool PlatformViewRegistry::HasFactory(const std::string& view_type) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return factories_.find(view_type) != factories_.end();
}

bool PlatformViewRegistry::CreateViaFactory(const CreateRequest& request) {
  Factory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = factories_.find(request.view_type);
    if (it == factories_.end()) {
      return false;  // no factory: caller falls back to the generated dispatch
    }
    factory = it->second;
  }

  // Invoke outside the lock: the factory calls RegisterListener, which locks.
  std::unique_ptr<PlatformView> instance = factory(*this, request);
  if (!instance) {
    ihs::log::warn("[PlatformViews] factory for '{}' returned no instance",
                   request.view_type);
    return true;  // the type was handled here; do not fall back
  }

  std::lock_guard<std::mutex> lock(mutex_);
  // The factory registered the callback table via RegisterListener, creating
  // the entry; attach ownership so dispose destroys it.
  instances_[request.id].owned = std::move(instance);
  return true;
}

void PlatformViewRegistry::RegisterListener(
    int32_t id,
    const platform_view_listener* listener,
    void* context) {
  std::unique_ptr<PlatformView> to_destroy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = instances_.find(id); it != instances_.end()) {
      // A repeated registration for a live id — the framework re-creating a
      // platform view with the same id before disposing the old one. The new
      // view wins: move the old owned instance out to destroy after the lock,
      // and replace the callback table + context in place so the entry stays
      // well-formed (the matching CreateViaFactory attaches the new owned
      // view). (Previously this erased the entry, which CreateViaFactory then
      // resurrected via operator[] with a null listener -> a later Resize/touch
      // dereferenced it and crashed.)
      to_destroy = std::move(it->second.owned);
      it->second.listener = listener;
      it->second.listener_context = context;
    } else {
      instances_[id] = Instance{listener, context, nullptr};
    }
  }
}

void PlatformViewRegistry::UnregisterListener(int32_t id) {
  std::unique_ptr<PlatformView> to_destroy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = instances_.find(id); it != instances_.end()) {
      to_destroy = std::move(it->second.owned);
      instances_.erase(it);
    }
  }
}

bool PlatformViewRegistry::LookupListener(
    int32_t id,
    const platform_view_listener** listener,
    void** context) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = instances_.find(id);
  // A well-formed instance always has a listener; treat a listener-less entry
  // as absent so callers (Resize/OnTouch/…) never dereference a null table.
  if (it == instances_.end() || it->second.listener == nullptr) {
    return false;
  }
  *listener = it->second.listener;
  *context = it->second.listener_context;
  return true;
}

bool PlatformViewRegistry::Dispose(int32_t id, bool hybrid) {
  Instance instance;
  bool had_instance = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (const auto it = instances_.find(id); it != instances_.end()) {
      instance = std::move(it->second);
      instances_.erase(it);
      had_instance = true;
    }
  }

  // Outside the lock: the dispose callback and the owned-instance dtor may
  // re-enter the registry (UnregisterListener).
  if (instance.listener != nullptr && instance.listener->dispose != nullptr) {
    instance.listener->dispose(hybrid, instance.listener_context);
  }

  // Safety net: drop the compositor surface even if the plugin did not, and
  // even when no listener was registered (matches the pre-registry handler).
  UnregisterCompositorSurface(id);

  // instance.owned (factory path) destructs here, outside the lock.
  return had_instance;
}

void PlatformViewRegistry::Resize(int32_t id, double width, double height) {
  const platform_view_listener* listener = nullptr;
  void* context = nullptr;
  if (LookupListener(id, &listener, &context) && listener->resize != nullptr) {
    listener->resize(width, height, context);
  }
  ResizeCompositorSurface(id, static_cast<int32_t>(width),
                          static_cast<int32_t>(height));
}

void PlatformViewRegistry::SetDirection(int32_t id, int32_t direction) {
  const platform_view_listener* listener = nullptr;
  void* context = nullptr;
  if (LookupListener(id, &listener, &context) &&
      listener->set_direction != nullptr) {
    listener->set_direction(direction, context);
  }
}

void PlatformViewRegistry::SetOffset(int32_t id, double left, double top) {
  const platform_view_listener* listener = nullptr;
  void* context = nullptr;
  if (LookupListener(id, &listener, &context) &&
      listener->set_offset != nullptr) {
    listener->set_offset(left, top, context);
  }
}

void PlatformViewRegistry::OnTouch(int32_t id,
                                   int32_t action,
                                   int32_t point_count,
                                   size_t pointer_data_size,
                                   const double* pointer_data) {
  const platform_view_listener* listener = nullptr;
  void* context = nullptr;
  if (LookupListener(id, &listener, &context) &&
      listener->on_touch != nullptr) {
    listener->on_touch(action, point_count, pointer_data_size, pointer_data,
                       context);
  }
}

void PlatformViewRegistry::AcceptGesture(int32_t id) {
  const platform_view_listener* listener = nullptr;
  void* context = nullptr;
  if (LookupListener(id, &listener, &context) &&
      listener->accept_gesture != nullptr) {
    listener->accept_gesture(id, context);
  }
}

void PlatformViewRegistry::RejectGesture(int32_t id) {
  const platform_view_listener* listener = nullptr;
  void* context = nullptr;
  if (LookupListener(id, &listener, &context) &&
      listener->reject_gesture != nullptr) {
    listener->reject_gesture(id, context);
  }
}

bool PlatformViewRegistry::Renegotiate(const int32_t id) {
  const platform_view_listener* listener = nullptr;
  void* context = nullptr;
  if (!LookupListener(id, &listener, &context) ||
      listener->renegotiate == nullptr) {
    return false;
  }
  listener->renegotiate(id, context);
  return true;
}

std::vector<int32_t> PlatformViewRegistry::InstanceIds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<int32_t> ids;
  ids.reserve(instances_.size());
  for (const auto& [id, instance] : instances_) {
    ids.push_back(id);
  }
  return ids;
}

void PlatformViewRegistry::RenegotiateAll() {
  // Snapshot the ids, then dispatch outside the lock: a plugin's renegotiate
  // re-runs ihs_pv_negotiate, which comes back through the registry, so
  // holding the lock across the callback would deadlock -- the same reason
  // LookupListener copies the table out rather than dispatching under it.
  for (const int32_t id : InstanceIds()) {
    (void)Renegotiate(id);
  }
}

void PlatformViewRegistry::UnregisterCompositorSurface(int32_t id) const {
#if BUILD_COMPOSITOR
  if (engine_state_ != nullptr && engine_state_->view_controller != nullptr &&
      engine_state_->view_controller->view != nullptr) {
    engine_state_->view_controller->view->UnregisterCompositorSurface(id);
  }
#else
  (void)engine_state_;
  (void)id;
#endif
}

void PlatformViewRegistry::ResizeCompositorSurface(int32_t id,
                                                   int32_t width,
                                                   int32_t height) const {
#if BUILD_COMPOSITOR
  if (engine_state_ != nullptr && engine_state_->view_controller != nullptr &&
      engine_state_->view_controller->view != nullptr) {
    engine_state_->view_controller->view->ResizeCompositorSurface(id, width,
                                                                  height);
  }
#else
  (void)engine_state_;
  (void)id;
  (void)width;
  (void)height;
#endif
}
