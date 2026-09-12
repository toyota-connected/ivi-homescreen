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

// Forwarders for the OSGi bundle handshake (see ihs/ihs_osgi.h).
//
// This library holds no OSGi state. The registry that backs these calls lives
// in the shell (shell/osgi/bridge_registry), which is an executable rather than
// a shared object a dlopen'd plugin can bind to -- the same situation the
// FlutterDesktop* forwarders exist for, and handled the same way: the shell
// installs a proc table, and every function here reads one atomic pointer and
// calls through it.
//
// Until the shell installs that table every call reports
// IHS_OSGI_ERR_UNAVAILABLE. That is the honest answer in a build with
// ENABLE_OSGI off, and in a process that is not ivi-homescreen at all.

#include "ihs/ihs_osgi.h"

#include "ihs/ihs_osgi_host.h"

#include <atomic>

namespace {

// Installed once by the shell; read on every forwarded call. Acquire/release
// pairs with the store so a bundle's isolate thread sees a fully-published
// table rather than a half-written one.
std::atomic<const IhsOsgiHost*> g_host{nullptr};

const IhsOsgiHost* host() {
  return g_host.load(std::memory_order_acquire);
}

}  // namespace

extern "C" void ihs_osgi_set_host(const IhsOsgiHost* host) {
  // Refuse a table too small to cover every entry the forwarders call through.
  // The struct only ever grows by appending, so a short one means an older
  // shell against a newer ihs_shared: the trailing entries would be absent
  // rather than NULL, and calling through them is undefined. Leaving the
  // forwarders on their safe defaults is degraded but well-defined.
  //
  // Unlike ihs_semantics_set_host the table is not copied -- it is read on
  // every call and must stay valid until uninstalled.
  if (host != nullptr && host->struct_size < sizeof(IhsOsgiHost)) {
    host = nullptr;
  }
  g_host.store(host, std::memory_order_release);
}

extern "C" bool ihs_osgi_available(void) {
  return host() != nullptr;
}

extern "C" int ihs_osgi_register_framework(const IhsOsgiPeerInfo* info,
                                           size_t* out_served) {
  // Validated before the host is consulted: a malformed argument is the
  // caller's mistake either way, and reporting it identically with and without
  // a host keeps the surface predictable.
  if (info == nullptr || info->struct_size < sizeof(IhsOsgiPeerInfo) ||
      info->port == 0) {
    return IHS_OSGI_ERR_INVALID;
  }
  const IhsOsgiHost* h = host();
  if (h == nullptr || h->register_framework == nullptr) {
    return IHS_OSGI_ERR_UNAVAILABLE;
  }
  return h->register_framework(h->user_data, info->dart_api_dl_data, info->port,
                               out_served);
}

extern "C" int ihs_osgi_register_bundle(const IhsOsgiBundleInfo* info,
                                        IhsOsgiBundle** out_bundle) {
  if (info == nullptr || info->struct_size < sizeof(IhsOsgiBundleInfo) ||
      info->peer.struct_size < sizeof(IhsOsgiPeerInfo) ||
      info->peer.port == 0 || info->symbolic_name == nullptr ||
      info->symbolic_name[0] == '\0' || out_bundle == nullptr) {
    return IHS_OSGI_ERR_INVALID;
  }
  // Cleared up front so a caller that ignores the status never reads a stale
  // handle out of its own uninitialized variable.
  *out_bundle = nullptr;
  const IhsOsgiHost* h = host();
  if (h == nullptr || h->register_bundle == nullptr) {
    return IHS_OSGI_ERR_UNAVAILABLE;
  }
  return h->register_bundle(h->user_data, info->peer.dart_api_dl_data,
                            info->peer.port, info->symbolic_name, out_bundle);
}

extern "C" int ihs_osgi_report_active(IhsOsgiBundle* bundle) {
  if (bundle == nullptr) {
    return IHS_OSGI_ERR_INVALID;
  }
  const IhsOsgiHost* h = host();
  if (h == nullptr || h->report_active == nullptr) {
    return IHS_OSGI_ERR_UNAVAILABLE;
  }
  return h->report_active(h->user_data, bundle);
}

extern "C" int ihs_osgi_report_stopped(IhsOsgiBundle* bundle) {
  if (bundle == nullptr) {
    return IHS_OSGI_ERR_INVALID;
  }
  const IhsOsgiHost* h = host();
  if (h == nullptr || h->report_stopped == nullptr) {
    return IHS_OSGI_ERR_UNAVAILABLE;
  }
  return h->report_stopped(h->user_data, bundle);
}

extern "C" int ihs_osgi_unregister(IhsOsgiBundle* bundle) {
  // Idempotent against a null handle, and deliberately OK rather than INVALID:
  // a teardown path that runs after a failed registration needs no special
  // case. Teardown is usually running because something else already failed,
  // and a second failure there buries the first.
  if (bundle == nullptr) {
    return IHS_OSGI_OK;
  }
  const IhsOsgiHost* h = host();
  if (h == nullptr || h->unregister == nullptr) {
    return IHS_OSGI_ERR_UNAVAILABLE;
  }
  return h->unregister(h->user_data, bundle);
}
