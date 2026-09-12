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

#include "osgi_host.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "ihs/ihs_osgi.h"
#include "ihs/ihs_osgi_host.h"

#include "bridge_registry.h"
#include "logging/logging.h"

namespace ihs::osgi {

namespace {

BridgeRegistry& RegistryOf(void* user_data) {
  return *static_cast<BridgeRegistry*>(user_data);
}

// The token, as the opaque pointer the C surface carries. Neither side ever
// dereferences it: it is a capability that happens to be pointer-sized, not an
// address, which is also why a forged one is harmless beyond being refused.
IhsOsgiBundle* ToHandle(const uint64_t token) {
  return reinterpret_cast<IhsOsgiBundle*>(static_cast<uintptr_t>(token));
}

uint64_t FromHandle(IhsOsgiBundle* bundle) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(bundle));
}

int64_t AddressOf(void* dart_api_dl_data) {
  return static_cast<int64_t>(reinterpret_cast<uintptr_t>(dart_api_dl_data));
}

int RegisterFramework(void* user_data,
                      void* dart_api_dl_data,
                      const int64_t port,
                      size_t* out_served) {
  BridgeRegistry& registry = RegistryOf(user_data);
  if (!registry.InitializeDartApi(AddressOf(dart_api_dl_data))) {
    return IHS_OSGI_ERR_DART_API;
  }
  const std::optional<size_t> served = registry.SetFrameworkPort(port);
  if (!served.has_value()) {
    return IHS_OSGI_ERR_REJECTED;
  }
  if (out_served != nullptr) {
    *out_served = *served;
  }
  ihs::log::debug("[osgi] ffi: framework registered; served {} bundle(s)",
                  *served);
  return IHS_OSGI_OK;
}

int RegisterBundle(void* user_data,
                   void* dart_api_dl_data,
                   const int64_t port,
                   const char* symbolic_name,
                   IhsOsgiBundle** out_bundle) {
  BridgeRegistry& registry = RegistryOf(user_data);
  if (!registry.InitializeDartApi(AddressOf(dart_api_dl_data))) {
    return IHS_OSGI_ERR_DART_API;
  }
  const std::string name(symbolic_name);
  if (!registry.RegisterBundle(name, port)) {
    return IHS_OSGI_ERR_REJECTED;
  }
  const uint64_t token = registry.MintHandle(name);
  if (token == 0) {
    // Unreachable unless the bundle vanished between the two calls. Either way
    // do not leave a registration holding the name with nothing able to report
    // through it -- that would burn the bundle's startup deadline for a reason
    // nothing logs.
    registry.UnregisterBundle(name);
    return IHS_OSGI_ERR_REJECTED;
  }
  *out_bundle = ToHandle(token);
  ihs::log::debug("[osgi] ffi: bundle '{}' registered", name);
  return IHS_OSGI_OK;
}

int ReportActive(void* user_data, IhsOsgiBundle* bundle) {
  BridgeRegistry& registry = RegistryOf(user_data);
  const std::optional<std::string> name =
      registry.BundleForHandle(FromHandle(bundle));
  if (!name.has_value()) {
    // Reachable by anything in the process that dlopens libihs_shared, so an
    // unrecognized handle is untrusted input to refuse rather than a reason to
    // crash -- and refusing it is the point of the handle existing.
    ihs::log::warn("[osgi] ffi: ACTIVE from an unrecognized handle");
    return IHS_OSGI_ERR_REJECTED;
  }
  return registry.ReportActive(*name) ? IHS_OSGI_OK : IHS_OSGI_ERR_REJECTED;
}

int ReportStopped(void* user_data, IhsOsgiBundle* bundle) {
  BridgeRegistry& registry = RegistryOf(user_data);
  const std::optional<std::string> name =
      registry.BundleForHandle(FromHandle(bundle));
  if (!name.has_value()) {
    ihs::log::warn("[osgi] ffi: STOPPED from an unrecognized handle");
    return IHS_OSGI_ERR_REJECTED;
  }
  return registry.ReportStopped(*name) ? IHS_OSGI_OK : IHS_OSGI_ERR_REJECTED;
}

int Unregister(void* user_data, IhsOsgiBundle* bundle) {
  BridgeRegistry& registry = RegistryOf(user_data);
  const std::optional<std::string> name =
      registry.BundleForHandle(FromHandle(bundle));
  if (!name.has_value()) {
    // Tolerated rather than refused: releasing what is already released is the
    // state the caller asked for, and teardown is usually running because
    // something else has already failed.
    return IHS_OSGI_OK;
  }
  registry.UnregisterBundle(*name);
  return IHS_OSGI_OK;
}

}  // namespace

void InstallOsgiHost(BridgeRegistry& registry) {
  // ihs_osgi_set_host does not copy the table -- it is read on every forwarded
  // call -- so it has to outlive the installation. A function-local static
  // does, and the registry it points at is itself immortal.
  static IhsOsgiHost host{};
  host.struct_size = sizeof(IhsOsgiHost);
  host.user_data = &registry;
  host.register_framework = &RegisterFramework;
  host.register_bundle = &RegisterBundle;
  host.report_active = &ReportActive;
  host.report_stopped = &ReportStopped;
  host.unregister = &Unregister;
  ihs_osgi_set_host(&host);
  ihs::log::debug("[osgi] ffi: host installed");
}

void UninstallOsgiHost() {
  ihs_osgi_set_host(nullptr);
}

}  // namespace ihs::osgi
