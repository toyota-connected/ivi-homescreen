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

#include "bridge_registry.h"

#include "dart_api_dl.h"

#include "logging/logging.h"

namespace ihs::osgi {

namespace {

bool PostInt64(const int64_t port, const int64_t value) {
  Dart_CObject message;
  message.type = Dart_CObject_kInt64;
  message.value.as_int64 = value;
  return Dart_PostCObject_DL(static_cast<Dart_Port_DL>(port), &message);
}

}  // namespace

const DartPortApi& RealDartPortApi() {
  static constexpr DartPortApi kApi{&Dart_InitializeApiDL, &PostInt64};
  return kApi;
}

BridgeRegistry::BridgeRegistry(const DartPortApi& api) : api_(api) {}

BridgeRegistry& BridgeRegistry::Instance() {
  static BridgeRegistry instance;
  return instance;
}

bool BridgeRegistry::InitializeDartApi(const int64_t dl_data_address) {
  std::lock_guard lock(mutex_);
  if (dart_api_ready_) {
    return true;
  }
  if (dl_data_address == 0) {
    ihs::log::error("[osgi] bridge: initializeApiDLData address is null");
    return false;
  }

  // Non-zero means these vendored headers disagree with the running VM. That is
  // a build-configuration error (a Flutter bump without re-pinning the Dart
  // headers), not a runtime condition to retry -- see PROVENANCE.md.
  if (const intptr_t rc = api_.initialize_api(
          reinterpret_cast<void*>(static_cast<uintptr_t>(dl_data_address)));
      rc != 0) {
    ihs::log::error(
        "[osgi] bridge: Dart_InitializeApiDL failed ({}); the vendored Dart DL "
        "headers do not match the running VM",
        rc);
    return false;
  }

  dart_api_ready_ = true;
  ihs::log::debug("[osgi] bridge: Dart DL API initialized");
  return true;
}

bool BridgeRegistry::dart_api_ready() const {
  std::lock_guard lock(mutex_);
  return dart_api_ready_;
}

bool BridgeRegistry::DeliverLocked(const std::string& symbolic_name,
                                   const int64_t port) {
  if (!framework_port_.has_value()) {
    return false;
  }
  if (!api_.post_int64(port, *framework_port_)) {
    // The port is closed or invalid: the isolate went away between registering
    // and now. Not fatal to the process -- the bundle is simply unreachable.
    ihs::log::error(
        "[osgi] bridge: failed to post the framework port to bundle '{}' "
        "(port {} closed?)",
        symbolic_name, port);
    return false;
  }
  return true;
}

std::optional<size_t> BridgeRegistry::SetFrameworkPort(
    const int64_t framework_port) {
  std::lock_guard lock(mutex_);
  if (!dart_api_ready_) {
    ihs::log::error(
        "[osgi] bridge: framework port set before Dart_InitializeApiDL");
    return std::nullopt;
  }
  if (framework_port == 0) {
    ihs::log::error("[osgi] bridge: framework port is not a valid Dart_Port");
    return std::nullopt;
  }
  if (framework_port_.has_value() && *framework_port_ != framework_port) {
    // Two framework isolates in one process is a startup bug, not a
    // configuration a bundle could recover from.
    ihs::log::error(
        "[osgi] bridge: framework port already set to {}; refusing to replace "
        "it with {}",
        *framework_port_, framework_port);
    return std::nullopt;
  }

  framework_port_ = framework_port;

  // Flush every bundle that registered before the framework existed. This is
  // the half of the race that makes registration order irrelevant.
  size_t served = 0;
  for (auto& [name, bundle] : bundles_) {
    if (bundle.framework_port_delivered) {
      continue;
    }
    if (DeliverLocked(name, bundle.port)) {
      bundle.framework_port_delivered = true;
      ++served;
    }
  }
  return served;
}

std::optional<int64_t> BridgeRegistry::framework_port() const {
  std::lock_guard lock(mutex_);
  return framework_port_;
}

bool BridgeRegistry::RegisterBundle(const std::string& symbolic_name,
                                    const int64_t port) {
  std::lock_guard lock(mutex_);
  if (!dart_api_ready_) {
    ihs::log::error(
        "[osgi] bridge: bundle '{}' registered before "
        "Dart_InitializeApiDL",
        symbolic_name);
    return false;
  }
  if (symbolic_name.empty()) {
    ihs::log::error("[osgi] bridge: bundle registered with an empty name");
    return false;
  }
  if (port == 0) {
    ihs::log::error("[osgi] bridge: bundle '{}' supplied an invalid port",
                    symbolic_name);
    return false;
  }
  if (bundles_.find(symbolic_name) != bundles_.end()) {
    // A restart must UnregisterBundle first; silently rebinding would strand
    // whichever isolate is no longer referenced.
    ihs::log::error("[osgi] bridge: bundle '{}' is already registered",
                    symbolic_name);
    return false;
  }

  Bundle bundle{port, false};
  // When the framework is already up, serve immediately -- the other half of
  // the race.
  if (framework_port_.has_value()) {
    bundle.framework_port_delivered = DeliverLocked(symbolic_name, port);
  }
  bundles_.emplace(symbolic_name, bundle);
  return true;
}

bool BridgeRegistry::UnregisterBundle(const std::string& symbolic_name) {
  std::lock_guard lock(mutex_);
  return bundles_.erase(symbolic_name) > 0;
}

std::optional<int64_t> BridgeRegistry::PortFor(
    const std::string& symbolic_name) const {
  std::lock_guard lock(mutex_);
  const auto it = bundles_.find(symbolic_name);
  if (it == bundles_.end()) {
    return std::nullopt;
  }
  return it->second.port;
}

std::vector<std::string> BridgeRegistry::PendingBundles() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> pending;
  for (const auto& [name, bundle] : bundles_) {
    if (!bundle.framework_port_delivered) {
      pending.push_back(name);
    }
  }
  return pending;
}

void BridgeRegistry::SetLifecycleObserver(IBundleLifecycleObserver* observer) {
  std::lock_guard lock(mutex_);
  observer_ = observer;
}

bool BridgeRegistry::ReportActive(const std::string& symbolic_name) {
  IBundleLifecycleObserver* observer = nullptr;
  {
    std::lock_guard lock(mutex_);
    if (bundles_.find(symbolic_name) == bundles_.end()) {
      // A report from a bundle that never called init: either a stale message
      // from a previous incarnation, or a symbolic_name that does not match the
      // config. Either way there is nothing to advance.
      ihs::log::warn("[osgi] bridge: ACTIVE from unregistered bundle '{}'",
                     symbolic_name);
      return false;
    }
    observer = observer_;
  }
  // Dispatched outside the lock: the observer is the orchestrator, whose
  // NotifyActive takes its own lock and signals a condition variable. Holding
  // this one across that call would couple two unrelated locks on the startup
  // path for no reason.
  if (observer != nullptr) {
    observer->OnBundleActive(symbolic_name);
  }
  return true;
}

bool BridgeRegistry::ReportStopped(const std::string& symbolic_name) {
  IBundleLifecycleObserver* observer = nullptr;
  {
    std::lock_guard lock(mutex_);
    observer = observer_;
  }
  if (observer != nullptr) {
    observer->OnBundleStopped(symbolic_name);
  }
  return true;
}

void BridgeRegistry::ResetForTesting() {
  std::lock_guard lock(mutex_);
  bundles_.clear();
  framework_port_.reset();
  observer_ = nullptr;
}

}  // namespace ihs::osgi
