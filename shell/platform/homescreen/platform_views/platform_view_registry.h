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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "platform_view.h"
#include "platform_view_listener.h"

struct FlutterDesktopEngineState;

// Owns the id -> instance lifecycle for platform views plus the view-type
// factory table. PlatformViewsHandler is a thin channel adapter over this
// registry, and the shared-module C API fronts the registry (not the handler):
// create/resize/dispose/touch/gesture all flow through one object.
//
// Two creation paths coexist:
//   - Legacy: the generated PluginsAoiPlatformViewCreate dispatch constructs a
//     plugin owned by the Flutter PluginRegistrar and registers its C callback
//     table here. The registry tracks the table but not the object's lifetime.
//   - Factory: a runtime factory (the seam the C API registers through) returns
//     an instance the registry owns and destroys on dispose.
//
// All methods run on the platform thread today; the mutex guards the maps for
// the C API's future callers. Listener callbacks and owned-instance destruction
// never run while the lock is held (a plugin dtor calls UnregisterListener,
// which re-enters), so the lock is always released before calling out.
class PlatformViewRegistry {
 public:
  // Parsed "create" request from the platform_views channel.
  struct CreateRequest {
    int32_t id{};
    std::string view_type;
    int32_t direction{};
    double left{};
    double top{};
    double width{};
    double height{};
    const std::vector<uint8_t>* params{};
  };

  // Constructs a PlatformView for a create request and returns the instance the
  // registry then owns (destroyed on dispose), or nullptr on failure. The
  // factory must register its callback table via RegisterListener before
  // returning. Runs on the platform thread.
  using Factory = std::function<std::unique_ptr<PlatformView>(
      PlatformViewRegistry& registry,
      const CreateRequest& request)>;

  explicit PlatformViewRegistry(FlutterDesktopEngineState* engine_state);
  // Defaulted inline (all members are complete types here) so the destructor is
  // emitted in every translation unit. FlutterDesktopEngineState holds this by
  // unique_ptr unconditionally, but the registry is only ever constructed under
  // BUILD_COMPOSITOR; an out-of-line dtor would leave the engine-state
  // destructor with an undefined reference in BUILD_COMPOSITOR=OFF builds.
  ~PlatformViewRegistry() = default;

  PlatformViewRegistry(const PlatformViewRegistry&) = delete;
  PlatformViewRegistry& operator=(const PlatformViewRegistry&) = delete;

  // View-type factories — the seam the shared-module C API registers through.
  void RegisterFactory(const std::string& view_type, Factory factory);
  void UnregisterFactory(const std::string& view_type);
  [[nodiscard]] bool HasFactory(const std::string& view_type) const;

  // Create through a registered factory. Returns true if a factory handled the
  // type (the instance is now registry-owned); false when none matches, so the
  // caller falls back to the legacy generated dispatch.
  bool CreateViaFactory(const CreateRequest& request);

  // Callback-table registration. The handler's Add/Remove trampolines and
  // factories call these. RegisterListener keeps the legacy toggle: a repeated
  // registration for a live id clears it (matches the pre-registry
  // PlatformViewAddListener). UnregisterListener drops the table only; the
  // instance may already be gone (a plugin dtor calls it late, at shutdown).
  void RegisterListener(int32_t id,
                        const platform_view_listener* listener,
                        void* context);
  void UnregisterListener(int32_t id);

  // Lifecycle dispatch, driven by the handler adapter (platform thread).
  // Dispose fires the plugin's dispose callback, drops the compositor surface,
  // erases the instance (destroying an owned one), and returns whether an
  // instance was present.
  bool Dispose(int32_t id, bool hybrid);
  void Resize(int32_t id, double width, double height);
  void SetDirection(int32_t id, int32_t direction);
  void SetOffset(int32_t id, double left, double top);
  void OnTouch(int32_t id,
               int32_t action,
               int32_t point_count,
               size_t pointer_data_size,
               const double* pointer_data);
  void AcceptGesture(int32_t id);
  void RejectGesture(int32_t id);

  // Tell one view its grant is stale. Returns false when no instance holds
  // @p id, or when the view registered no renegotiate callback -- the caller
  // uses that to tell "nothing to notify" from "notified".
  bool Renegotiate(int32_t id);

  // Tell every live view. The output layer calls this for the views bound to
  // an output that changed; a caller with no per-view mapping (a single-output
  // system) can use it directly.
  void RenegotiateAll();

  // Ids of the live instances, so a caller can decide which to notify.
  [[nodiscard]] std::vector<int32_t> InstanceIds() const;

 private:
  struct Instance {
    const platform_view_listener* listener{nullptr};
    void* listener_context{nullptr};
    // Non-null only for factory-created views: the registry owns and destroys
    // them on dispose. Legacy views are owned by the Flutter PluginRegistrar,
    // so this stays null and their lifetime is unchanged until they migrate.
    std::unique_ptr<PlatformView> owned;
  };

  // Copies out the callback table for @p id under the lock so the caller can
  // invoke it after releasing the lock. Returns false when no instance exists.
  bool LookupListener(int32_t id,
                      const platform_view_listener** listener,
                      void** context) const;

  // Drops the compositor surface for @p id if a view is attached (safety net,
  // mirrors the pre-registry handler). No-op in headless mode.
  void UnregisterCompositorSurface(int32_t id) const;
  void ResizeCompositorSurface(int32_t id, int32_t width, int32_t height) const;

  // Only referenced under BUILD_COMPOSITOR (the compositor-surface safety
  // nets); the registry source propagates to plugin targets that build without
  // it.
  [[maybe_unused]] FlutterDesktopEngineState* engine_state_;
  mutable std::mutex mutex_;
  std::map<std::string, Factory> factories_;
  std::map<int32_t, Instance> instances_;
};
