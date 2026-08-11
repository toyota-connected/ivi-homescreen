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
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ihs::osgi {

// Seam over the two Dart DL entry points the bridge needs.
//
// The registry is pure bookkeeping around these two calls, and both require a
// live Dart VM. Routing them through a struct of function pointers lets the
// bookkeeping -- which is where the ordering hazard lives -- be tested without
// one. Production code uses RealDartPortApi(); tests substitute fakes.
struct DartPortApi {
  // Wraps Dart_InitializeApiDL. Returns 0 on success, non-zero on version
  // mismatch between these headers and the running VM.
  intptr_t (*initialize_api)(void* data);

  // Wraps Dart_PostCObject_DL with an int64 payload. Returns true when the
  // message was enqueued; false when @port is closed or invalid.
  bool (*post_int64)(int64_t port, int64_t value);
};

// The real Dart DL calls.
const DartPortApi& RealDartPortApi();

// Receives bundle lifecycle reports that arrive from Dart over the bridge.
//
// The bridge is per-engine and knows nothing about startup sequencing; the
// orchestrator owns the sequencing and knows nothing about channels. This is
// the seam between them -- without it a bundle can register its port and still
// never be seen to reach ACTIVE, so every critical bundle times out no matter
// how correctly its activator behaves.
class IBundleLifecycleObserver {
 public:
  virtual ~IBundleLifecycleObserver() = default;

  // The bundle's activator finished start(). This is what ACTIVE means in the
  // OSGi lifecycle -- not that the engine is up, which the shell already knows,
  // but that the bundle's own code declared itself ready.
  virtual void OnBundleActive(const std::string& symbolic_name) = 0;

  // The bundle's activator finished stop(), or it is going away.
  virtual void OnBundleStopped(const std::string& symbolic_name) = 0;
};

// Tracks which isolate ports the OSGi bridge knows about and hands each bundle
// the framework isolate's port.
//
// The ordering hazard this exists to absorb: a bundle engine and the framework
// isolate start concurrently, so a bundle's `init` can arrive either before or
// after the framework has registered its port. Rather than require one order
// and race, registration is commutative -- whichever arrives second triggers
// delivery, and a bundle registered early is recorded as pending and served the
// moment the framework port is known. Delivery therefore never depends on
// startup order.
class BridgeRegistry {
 public:
  explicit BridgeRegistry(const DartPortApi& api = RealDartPortApi());

  BridgeRegistry(const BridgeRegistry&) = delete;
  BridgeRegistry& operator=(const BridgeRegistry&) = delete;

  // Process-wide instance used by OsgiBridgePlugin.
  static BridgeRegistry& Instance();

  // Bind these headers' DL symbol table to the running VM, from the address of
  // Dart's NativeApi.initializeApiDLData. Idempotent: the data is a VM-global,
  // identical for every isolate in the process (all Flutter engines here share
  // one VM), so only the first call does work and later calls re-report it.
  //
  // Returns false on a version mismatch, having logged it. Nothing else on this
  // class will work until this succeeds.
  bool InitializeDartApi(int64_t dl_data_address);

  [[nodiscard]] bool dart_api_ready() const;

  // Record the framework isolate's receive port and flush it to every bundle
  // that registered before it was known. Returns the number of bundles served,
  // or nullopt if the DL API is not initialized.
  std::optional<size_t> SetFrameworkPort(int64_t framework_port);

  [[nodiscard]] std::optional<int64_t> framework_port() const;

  // Record a bundle's receive port. When the framework port is already known it
  // is delivered immediately; otherwise the bundle is served by the next
  // SetFrameworkPort. Returns false on a duplicate symbolic name, an invalid
  // port, or an uninitialized DL API.
  bool RegisterBundle(const std::string& symbolic_name, int64_t port);

  // Forget a bundle, so a restart can re-register the same symbolic name.
  // Returns true if it was known.
  bool UnregisterBundle(const std::string& symbolic_name);

  [[nodiscard]] std::optional<int64_t> PortFor(
      const std::string& symbolic_name) const;

  // Bundles registered but not yet handed the framework port, in registration
  // order. Empty once the framework port is known.
  [[nodiscard]] std::vector<std::string> PendingBundles() const;

  // Route lifecycle reports to @observer. Null detaches. The observer must
  // outlive the registry's use of it; in practice it is the orchestrator, which
  // lives for the whole run.
  void SetLifecycleObserver(IBundleLifecycleObserver* observer);

  // A bundle reported that its activator completed. Returns false when the
  // bundle is unknown to the bridge, which means it never called init.
  bool ReportActive(const std::string& symbolic_name);

  // A bundle reported that it stopped.
  bool ReportStopped(const std::string& symbolic_name);

  // Test seam: drop all state (not the DL binding, which is a VM-global).
  void ResetForTesting();

 private:
  // Caller holds mutex_.
  bool DeliverLocked(const std::string& symbolic_name, int64_t port);

  const DartPortApi& api_;

  mutable std::mutex mutex_;
  bool dart_api_ready_{false};
  std::optional<int64_t> framework_port_;
  IBundleLifecycleObserver* observer_{nullptr};

  struct Bundle {
    int64_t port{0};
    bool framework_port_delivered{false};
  };
  // Ordered so PendingBundles() is deterministic, which keeps the startup log
  // reproducible across runs.
  std::map<std::string, Bundle> bundles_;
};

}  // namespace ihs::osgi
