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

#include "backend/wayland_leased_drm/lease_client.h"

#include "logging/logging.h"

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

extern "C" {
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xf86drm.h>
}

#include "wayland-protocols/drm_lease_v1_client.hpp"

namespace homescreen {
namespace {

namespace lease_proto = drm_lease_v1::client;
using Clock = std::chrono::steady_clock;

// Bound on the wp_drm_lease_device_v1.release -> `released` handshake at
// teardown. The compositor is required to answer, but teardown must not hang on
// a wedged one: past this we destroy the proxy unilaterally.
constexpr int kReleaseDrainMs = 250;

// A live Wayland session = WAYLAND_DISPLAY set AND the socket actually exists
// (the env var alone can be stale). Mirrors WaylandSessionAvailable() in
// register_backends.cc; kept local so the lease client does not depend on the
// registry translation unit. Later, hoist the pair into one shared helper.
bool WaylandSessionAvailable() {
  const char* wl = std::getenv("WAYLAND_DISPLAY");
  if (wl == nullptr || *wl == '\0') {
    return false;
  }
  std::error_code ec;
  const std::filesystem::path sock(wl);
  if (sock.is_absolute()) {
    return std::filesystem::exists(sock, ec);
  }
  const char* xdg = std::getenv("XDG_RUNTIME_DIR");
  if (xdg == nullptr || *xdg == '\0') {
    return true;  // can't resolve the socket path; trust the env var
  }
  return std::filesystem::exists(std::filesystem::path(xdg) / sock, ec);
}

std::string NodePathFromFd(int fd) {
  if (fd < 0) {
    return {};
  }
  char* n = drmGetDeviceNameFromFd2(fd);
  if (n == nullptr) {
    return {};
  }
  std::string out(n);
  free(n);
  return out;
}

// ── proxies ────────────────────────────────────────────────────────────────
//
// wl::CProxy is a NON-owning handle and ~CProxyImpl is a defaulted no-op, so
// every proxy this file creates must be destroyed explicitly. That is not just
// a leak concern: _SetProxy installs `this` as the dispatcher's
// `implementation` pointer, so a C++ object outliving its proxy leaves the
// compositor able to dispatch an event into freed memory. Each class below
// therefore destroys its own proxy in its destructor, which keeps object and
// proxy lifetimes welded together no matter which path frees them.

class Connector : public lease_proto::CWpDrmLeaseConnectorV1<Connector> {
 public:
  LeaseOffer offer;
  bool done = false;
  bool withdrawn = false;

  ~Connector() override { DestroyProxy(); }

  // wp_drm_lease_connector_v1.destroy IS a destructor request, so the generated
  // Destroy() marshals it and destroys the proxy. Detach() then yields null
  // here, making this idempotent.
  void DestroyProxy() noexcept {
    if (GetProxy() != nullptr) {
      Destroy();
    }
  }

  void OnName(const char* v) override { offer.name = v != nullptr ? v : ""; }
  void OnDescription(const char* v) override {
    offer.description = v != nullptr ? v : "";
  }
  void OnConnectorId(uint32_t v) override { offer.connector_id = v; }
  void OnDone() override { done = true; }

  // Post-grant this does NOT revoke: the spec states the lease status is
  // unchanged and the client should merely destroy the object. Pre-grant it
  // means the offer evaporated before we could request it.
  void OnWithdrawn() override { withdrawn = true; }
};

class Device : public lease_proto::CWpDrmLeaseDeviceV1<Device> {
 public:
  uint32_t global_name = 0;
  int drm_fd = -1;  // enumeration-only, NON-master; owned by us once dispatched
  std::string card_path;
  bool done = false;
  bool released = false;

  std::deque<std::unique_ptr<Connector>> connectors;

  ~Device() override {
    CloseDrmFd();
    // Children first: their proxies are independent objects, and dropping them
    // before the device keeps the teardown order obvious.
    connectors.clear();
    DestroyProxy();
  }

  void CloseDrmFd() noexcept {
    if (drm_fd >= 0) {
      close(drm_fd);
      drm_fd = -1;
    }
  }

  // Unlike the connector, wp_drm_lease_device_v1.release is NOT a destructor
  // request (only the `released` event is), so the generated Release() marshals
  // without destroying anything. Callers should prefer ReleaseAndDestroy();
  // this is the unilateral fallback.
  void DestroyProxy() noexcept {
    if (wl_proxy* p = Detach()) {
      wl_proxy_destroy(p);
    }
  }

  void OnDrmFd(int32_t fd) override {
    // The protocol permits repeat drm_fd events (e.g. after the compositor
    // regains DRM master); each supersedes the last, so close the old one
    // rather than dropping it on the floor.
    CloseDrmFd();
    drm_fd = fd;
    card_path = NodePathFromFd(fd);
  }

  void OnConnector(wl_proxy* id) override {
    if (id == nullptr) {
      return;
    }
    auto c = std::make_unique<Connector>();
    c->_SetProxy(id);
    connectors.emplace_back(std::move(c));
  }

  void OnDone() override { done = true; }
  void OnReleased() override { released = true; }
};

// wp_drm_lease_request_v1 has no events; it exists only to carry
// request_connector + submit.
class Request : public lease_proto::CWpDrmLeaseRequestV1<Request> {
 public:
  ~Request() override {
    if (wl_proxy* p = Detach()) {
      wl_proxy_destroy(p);
    }
  }
};

class Lease : public lease_proto::CWpDrmLeaseV1<Lease> {
 public:
  int lease_fd = -1;
  bool finished = false;

  ~Lease() override {
    // wp_drm_lease_v1.destroy IS a destructor request.
    if (GetProxy() != nullptr) {
      Destroy();
    }
  }

  void OnLeaseFd(int32_t fd) override { lease_fd = fd; }

  // Either a denial (pre-grant) or a revocation (post-grant, at any time).
  void OnFinished() override { finished = true; }
};

// The registry listener must outlive the registry proxy, so it has static
// storage and its user_data points at a Session field — never at a stack local.
struct RegistryScan {
  struct Found {
    uint32_t name;
    uint32_t version;
  };
  std::vector<Found> found;
};

const wl_registry_listener kRegistryListener = {
    [](void* ud, wl_registry*, uint32_t name, const char* iface, uint32_t ver) {
      if (std::string_view(iface) ==
          lease_proto::wp_drm_lease_device_v1_traits::interface_name) {
        static_cast<RegistryScan*>(ud)->found.push_back({name, ver});
      }
    },
    [](void*, wl_registry*, uint32_t) {},
};

// Dispatch until `pred` holds or the deadline passes. Returns false on timeout
// or connection error. Uses poll() rather than wl_display_dispatch() so the
// deadline is actually enforced — a bare dispatch would block indefinitely
// against a compositor that has deferred drm_fd while it lacks DRM master.
bool DispatchUntil(wl_display* display,
                   const std::function<bool()>& pred,
                   Clock::time_point deadline,
                   bool* protocol_error) {
  while (!pred()) {
    while (wl_display_prepare_read(display) != 0) {
      if (wl_display_dispatch_pending(display) < 0) {
        *protocol_error = true;
        return false;
      }
      if (pred()) {
        return true;
      }
    }
    // Read lock held from here: every exit below must read or cancel.

    // A short write (EAGAIN) leaves requests queued. Poll for writability so we
    // can flush the remainder — without POLLOUT the loop would never learn the
    // socket drained and would stall to the deadline with `submit` unsent.
    bool want_write = false;
    if (wl_display_flush(display) < 0) {
      if (errno == EAGAIN) {
        want_write = true;
      } else {
        wl_display_cancel_read(display);
        *protocol_error = true;
        return false;
      }
    }

    const auto now = Clock::now();
    if (now >= deadline) {
      wl_display_cancel_read(display);
      return false;
    }
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
            .count();

    pollfd pfd{wl_display_get_fd(display),
               static_cast<short>(POLLIN | (want_write ? POLLOUT : 0)), 0};
    const int r = poll(&pfd, 1, static_cast<int>(ms));
    if (r < 0) {
      wl_display_cancel_read(display);
      if (errno == EINTR) {
        continue;
      }
      *protocol_error = true;
      return false;
    }
    if (r == 0) {  // deadline expired
      wl_display_cancel_read(display);
      return false;
    }
    if ((pfd.revents & POLLIN) != 0) {
      if (wl_display_read_events(display) < 0) {
        *protocol_error = true;
        return false;
      }
      if (wl_display_dispatch_pending(display) < 0) {
        *protocol_error = true;
        return false;
      }
      continue;
    }
    // No data to read.
    wl_display_cancel_read(display);
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      *protocol_error = true;
      return false;
    }
    // POLLOUT only: loop round to re-flush the queued requests.
  }
  return true;
}

// Deadline-bounded wl_display_roundtrip. The stock roundtrip blocks forever,
// which would defeat LeaseConfig::timeout_ms against a frozen compositor.
bool BoundedRoundtrip(wl_display* display,
                      Clock::time_point deadline,
                      bool* protocol_error) {
  wl_callback* cb = wl_display_sync(display);
  if (cb == nullptr) {
    *protocol_error = true;
    return false;
  }
  bool done = false;
  static const wl_callback_listener listener = {
      [](void* ud, wl_callback*, uint32_t) { *static_cast<bool*>(ud) = true; },
  };
  wl_callback_add_listener(cb, &listener, &done);

  const bool ok = DispatchUntil(
      display, [&done]() { return done; }, deadline, protocol_error);
  wl_callback_destroy(cb);
  return ok;
}

// Connect, bind every advertised lease device, and let the initial enumeration
// burst land. Owns the connection until a caller takes it over.
struct Session {
  wl_display* display = nullptr;
  wl_registry* registry = nullptr;
  RegistryScan scan;
  std::deque<std::unique_ptr<Device>> devices;

  // Destroy the registry as soon as enumeration is done: its listener's
  // user_data points into this Session, so the proxy must never outlive it.
  // Nothing post-negotiation needs registry events — revocation arrives on
  // wp_drm_lease_v1.finished. Reacquire re-obtains one to see
  // connector re-offers.
  void CloseRegistry() {
    if (registry != nullptr) {
      wl_registry_destroy(registry);
      registry = nullptr;
    }
  }

  ~Session() {
    CloseRegistry();
    // Device/Connector destructors close the enumeration fd and destroy their
    // proxies, so every error path is covered without per-path cleanup.
    devices.clear();
    if (display != nullptr) {
      wl_display_disconnect(display);
    }
  }
};

// Politely hand a device back: release -> drain until `released` -> destroy.
// Falls back to destroying the proxy outright if the compositor doesn't answer,
// so this can never hang or leave a live dispatcher on a dying object.
void ReleaseAndDestroy(Device& dev, wl_display* display) {
  if (dev.GetProxy() == nullptr) {
    return;
  }
  // Connectors belong to the device; drop them before releasing it.
  dev.connectors.clear();
  dev.CloseDrmFd();

  dev.Release();
  bool protocol_error = false;
  const auto deadline =
      Clock::now() + std::chrono::milliseconds(kReleaseDrainMs);
  if (!DispatchUntil(
          display, [&dev]() { return dev.released; }, deadline,
          &protocol_error)) {
    ihs::log::debug(
        "[leased-drm] no `released` from compositor within {} ms; destroying "
        "the device proxy anyway",
        kReleaseDrainMs);
  }
  dev.DestroyProxy();
}

bool OpenSession(Session& s, Clock::time_point deadline, LeaseError* err) {
  if (!WaylandSessionAvailable()) {
    *err = LeaseError::kNoWaylandSession;
    return false;
  }
  s.display = wl_display_connect(nullptr);
  if (s.display == nullptr) {
    *err = LeaseError::kConnectFailed;
    return false;
  }

  s.registry = wl_display_get_registry(s.display);
  wl_registry_add_listener(s.registry, &kRegistryListener, &s.scan);

  // A failed roundtrip means the connection died — reporting that as
  // kNoLeaseDevice would tell the operator to add compositor support that is
  // already present, so surface it as the protocol error it is.
  bool protocol_error = false;
  if (!BoundedRoundtrip(s.display, deadline, &protocol_error)) {
    *err = protocol_error ? LeaseError::kProtocolError : LeaseError::kTimeout;
    return false;
  }

  if (s.scan.found.empty()) {
    *err = LeaseError::kNoLeaseDevice;
    return false;
  }

  // One global per DRM node — bind them all, then select (multi-GPU).
  for (const auto& f : s.scan.found) {
    auto* proxy = static_cast<wl_proxy*>(wl_registry_bind(
        s.registry, f.name,
        &lease_proto::wp_drm_lease_device_v1_traits::wl_iface(), 1));
    if (proxy == nullptr) {
      continue;
    }
    auto d = std::make_unique<Device>();
    d->global_name = f.name;
    d->_SetProxy(proxy);
    s.devices.emplace_back(std::move(d));
  }
  if (s.devices.empty()) {
    *err = LeaseError::kNoLeaseDevice;
    return false;
  }

  const auto all_done = [&s]() {
    for (const auto& d : s.devices) {
      if (!d->done) {
        return false;
      }
    }
    return true;
  };
  if (!DispatchUntil(s.display, all_done, deadline, &protocol_error)) {
    *err = protocol_error ? LeaseError::kProtocolError : LeaseError::kTimeout;
    return false;
  }

  // Each connector's own name/description/connector_id/done events follow the
  // device's new_id; one more (bounded) roundtrip lets them land.
  if (!BoundedRoundtrip(s.display, deadline, &protocol_error)) {
    *err = protocol_error ? LeaseError::kProtocolError : LeaseError::kTimeout;
    return false;
  }

  *err = LeaseError::kNone;
  return true;
}

std::vector<LeaseOffer> CollectOffers(const Session& s) {
  std::vector<LeaseOffer> out;
  for (const auto& d : s.devices) {
    for (const auto& c : d->connectors) {
      if (!c->withdrawn) {
        out.push_back(c->offer);
      }
    }
  }
  return out;
}

// Either a decimal index into the advertised order, or a DRM node path matched
// against the path derived from each device's enumeration drm_fd.
Device* SelectDevice(const Session& s,
                     const LeaseConfig& cfg,
                     LeaseError* err) {
  if (!cfg.device.has_value()) {
    if (s.devices.size() == 1) {
      return s.devices.front().get();
    }
    *err = LeaseError::kDeviceAmbiguous;
    return nullptr;
  }
  const std::string& want = *cfg.device;

  if (!want.empty() &&
      want.find_first_not_of("0123456789") == std::string::npos) {
    const auto idx =
        static_cast<size_t>(std::strtoul(want.c_str(), nullptr, 10));
    if (idx < s.devices.size()) {
      return s.devices[idx].get();
    }
    *err = LeaseError::kDeviceNotFound;
    return nullptr;
  }

  for (const auto& d : s.devices) {
    if (!d->card_path.empty() && d->card_path == want) {
      return d.get();
    }
  }
  *err = LeaseError::kDeviceNotFound;
  return nullptr;
}

Connector* SelectConnector(const Device& dev,
                           const LeaseConfig& cfg,
                           LeaseError* err) {
  std::vector<Connector*> live;
  for (const auto& c : dev.connectors) {
    if (!c->withdrawn) {
      live.push_back(c.get());
    }
  }
  if (live.empty()) {
    *err = LeaseError::kNoOffers;
    return nullptr;
  }
  if (!cfg.connector.has_value()) {
    if (live.size() == 1) {
      return live.front();
    }
    *err = LeaseError::kConnectorAmbiguous;
    return nullptr;
  }
  for (auto* c : live) {
    if (c->offer.name == *cfg.connector) {
      return c;
    }
  }
  *err = LeaseError::kConnectorNotFound;
  return nullptr;
}

void LogOffers(const std::vector<LeaseOffer>& offers) {
  for (const auto& o : offers) {
    ihs::log::info("[leased-drm]   offer: {} (connector_id {}) — {}", o.name,
                   o.connector_id, o.description);
  }
}

}  // namespace

const char* ToString(LeaseError e) {
  switch (e) {
    case LeaseError::kNone:
      return "ok";
    case LeaseError::kNoWaylandSession:
      return "no Wayland session";
    case LeaseError::kConnectFailed:
      return "wl_display_connect failed";
    case LeaseError::kNoLeaseDevice:
      return "compositor does not implement drm-lease-v1";
    case LeaseError::kDeviceAmbiguous:
      return "several lease devices advertised and none selected";
    case LeaseError::kDeviceNotFound:
      return "configured lease device not found";
    case LeaseError::kNoOffers:
      return "no connectors offered for leasing";
    case LeaseError::kConnectorAmbiguous:
      return "several connectors offered and none selected";
    case LeaseError::kConnectorNotFound:
      return "configured connector not offered";
    case LeaseError::kDenied:
      return "compositor denied the lease";
    case LeaseError::kTimeout:
      return "timed out negotiating the lease";
    case LeaseError::kProtocolError:
      return "Wayland protocol error";
  }
  return "unknown";
}

// ── LeaseHold ──────────────────────────────────────────────────────────────

// Behind a pimpl so the header stays free of wayland-client.h and the generated
// protocol header.
struct LeaseHold::Impl {
  wl_display* display = nullptr;
  std::unique_ptr<Device> device;
  std::unique_ptr<Lease> lease;
  std::thread monitor;

  // Teardown signal. `stop` is authoritative; stop_fd is only the wake-up
  // mechanism that gets the monitor out of a blocking poll() promptly. If
  // eventfd() fails we fall back to a polling tick, so teardown can never
  // depend on the fd having been created — otherwise join() would hang forever.
  std::atomic<bool> stop{false};
  int stop_fd = -1;
};

// Poll tick used only when eventfd() was unavailable, so the monitor still
// notices `stop` within a bounded time.
constexpr int kMonitorFallbackTickMs = 200;

LeaseHold::LeaseHold() : impl_(std::make_unique<Impl>()) {}

LeaseHold::~LeaseHold() {
  // The monitor is dispatching on the display we are about to tear down, so it
  // must be stopped and joined first. Everything after this runs
  // single-threaded.
  if (impl_->monitor.joinable()) {
    impl_->stop.store(true, std::memory_order_release);
    if (impl_->stop_fd >= 0) {
      const uint64_t one = 1;
      [[maybe_unused]] const ssize_t w =
          write(impl_->stop_fd, &one, sizeof(one));
    }
    impl_->monitor.join();
  }
  if (impl_->stop_fd >= 0) {
    close(impl_->stop_fd);
    impl_->stop_fd = -1;
  }

  // Teardown is destroy + close, NOT drmDropMaster: the lease fd is master over
  // its object set by construction and the compositor is the lessor. Destroying
  // the lease object is what tells it to drmModeRevokeLease and re-advertise
  // the connector.
  impl_->lease.reset();  // ~Lease sends destroy and drops the proxy
  if (lease_fd_ >= 0) {
    close(lease_fd_);
    lease_fd_ = -1;
  }
  if (impl_->device && impl_->display != nullptr) {
    ReleaseAndDestroy(*impl_->device, impl_->display);
  }
  impl_->device.reset();
  if (impl_->display != nullptr) {
    wl_display_flush(impl_->display);
    wl_display_disconnect(impl_->display);
    impl_->display = nullptr;
  }
}

// ── LeaseClient ────────────────────────────────────────────────────────────

LeaseClient::Result LeaseClient::Probe(const LeaseConfig& cfg) {
  Result res;
  Session s;
  const auto deadline =
      Clock::now() + std::chrono::milliseconds(cfg.timeout_ms);

  if (!OpenSession(s, deadline, &res.error)) {
    return res;
  }
  s.CloseRegistry();

  res.offers = CollectOffers(s);
  for (const auto& d : s.devices) {
    ihs::log::info("[leased-drm] device[{}] node={} connectors={}",
                   d->global_name,
                   d->card_path.empty() ? "(unresolved)" : d->card_path,
                   d->connectors.size());
  }
  LogOffers(res.offers);

  // Hand every device back politely — a probe must leave the compositor's view
  // exactly as it found it. ~Session would destroy the proxies regardless, but
  // without the release handshake.
  for (const auto& d : s.devices) {
    ReleaseAndDestroy(*d, s.display);
  }

  // Probe never negotiates, so `hold` is always null and cannot signal success;
  // kNone means "enumeration succeeded, see offers".
  res.error = res.offers.empty() ? LeaseError::kNoOffers : LeaseError::kNone;
  return res;
}

LeaseClient::Result LeaseClient::Acquire(const LeaseConfig& cfg) {
  Result res;
  Session s;
  const auto deadline =
      Clock::now() + std::chrono::milliseconds(cfg.timeout_ms);

  if (!OpenSession(s, deadline, &res.error)) {
    return res;
  }
  s.CloseRegistry();
  res.offers = CollectOffers(s);

  Device* dev = SelectDevice(s, cfg, &res.error);
  if (dev == nullptr) {
    LogOffers(res.offers);
    return res;
  }
  Connector* conn = SelectConnector(*dev, cfg, &res.error);
  if (conn == nullptr) {
    LogOffers(res.offers);
    return res;
  }

  ihs::log::info("[leased-drm] requesting connector '{}' (id {}) on {}",
                 conn->offer.name, conn->offer.connector_id,
                 dev->card_path.empty() ? "(unresolved)" : dev->card_path);

  // create_lease_request -> request_connector -> submit. `submit` destroys the
  // request object, so nothing may be sent on it afterwards. The requested set
  // is only a suggestion: the compositor picks the final object set.
  auto* req_proxy = wl::construct<
      lease_proto::wp_drm_lease_request_v1_traits,
      lease_proto::wp_drm_lease_device_v1_traits::Op::CreateLeaseRequest>(*dev);
  if (req_proxy == nullptr) {
    res.error = LeaseError::kProtocolError;
    return res;
  }
  auto lease = std::make_unique<Lease>();
  {
    Request request;
    // Attach, not _SetProxy: wp_drm_lease_request_v1 declares no events, so the
    // generated class has no _Dispatch to install a dispatcher for.
    request.Attach(req_proxy);
    request.RequestConnector(conn->GetProxy());

    auto* lease_proxy =
        wl::construct<lease_proto::wp_drm_lease_v1_traits,
                      lease_proto::wp_drm_lease_request_v1_traits::Op::Submit>(
            request);
    // submit consumed the request server-side; ~Request drops our proxy so it
    // can't be reused (sending on it would be a protocol error).
    if (lease_proxy == nullptr) {
      res.error = LeaseError::kProtocolError;
      return res;
    }
    lease->_SetProxy(lease_proxy);
  }

  // Terminal states: lease_fd (granted) or finished (denied).
  bool protocol_error = false;
  const auto settled = [&lease]() {
    return lease->lease_fd >= 0 || lease->finished;
  };
  if (!DispatchUntil(s.display, settled, deadline, &protocol_error)) {
    res.error =
        protocol_error ? LeaseError::kProtocolError : LeaseError::kTimeout;
    return res;
  }
  if (lease->lease_fd < 0) {
    res.error = LeaseError::kDenied;
    return res;
  }

  // Granted. The enumeration fd has served its purpose — the lease fd is the
  // master one from here on.
  dev->CloseDrmFd();

  auto hold = std::unique_ptr<LeaseHold>(new LeaseHold());
  hold->lease_fd_ = lease->lease_fd;
  hold->connector_id_ = conn->offer.connector_id;
  hold->connector_name_ = conn->offer.name;
  // Derive the node path from the LEASE fd — the fd every renderer will use.
  hold->card_path_ = NodePathFromFd(lease->lease_fd);
  if (hold->card_path_.empty()) {
    hold->card_path_ = dev->card_path;
  }

  // Hand the connection over; Session must not tear it down. Move the leased
  // device across FIRST, then release the rest while the display is still ours.
  hold->impl_->display = s.display;
  hold->impl_->lease = std::move(lease);
  for (auto& d : s.devices) {
    if (d.get() == dev) {
      hold->impl_->device = std::move(d);
      break;
    }
  }
  // Devices we did not lease from: hand them back properly. This must destroy
  // their proxies, not merely free the C++ objects — the display now lives on
  // the monitor thread, and a surviving proxy would dispatch `released` into
  // freed memory.
  for (auto& d : s.devices) {
    if (d) {
      ReleaseAndDestroy(*d, s.display);
    }
  }
  s.devices.clear();
  s.display = nullptr;

  ihs::log::info(
      "[leased-drm] lease granted: connector '{}' (id {}) fd={} node={}",
      hold->connector_name_, hold->connector_id_, hold->lease_fd_,
      hold->card_path_.empty() ? "(unresolved)" : hold->card_path_);

  // `finished` may already have landed in the same dispatch batch as lease_fd
  // (a VT switch arriving during startup). The monitor only re-reads it after a
  // fresh dispatch, and a dead lease produces no further events — so latch it
  // here or revoked() would stay false forever against dead KMS objects.
  if (hold->impl_->lease->finished) {
    ihs::log::warn(
        "[leased-drm] lease revoked before it could be used (finished arrived "
        "with lease_fd) on connector '{}' (id {})",
        hold->connector_name_, hold->connector_id_);
    hold->revoked_.store(true, std::memory_order_release);
  }

  // The event loop must stay alive for the process lifetime to observe
  // `finished` — every VT switch away from the compositor is a revoke.
  hold->impl_->stop_fd = eventfd(0, EFD_CLOEXEC);
  LeaseHold* h = hold.get();
  hold->impl_->monitor = std::thread([h]() {
    wl_display* display = h->impl_->display;
    Lease* lease_obj = h->impl_->lease.get();
    const int stop_fd = h->impl_->stop_fd;

    // Block indefinitely when the eventfd can wake us; otherwise tick so `stop`
    // is still observed promptly.
    const int poll_timeout = stop_fd >= 0 ? -1 : kMonitorFallbackTickMs;
    const nfds_t nfds = stop_fd >= 0 ? 2 : 1;

    for (;;) {
      if (h->impl_->stop.load(std::memory_order_acquire)) {
        return;  // orderly teardown
      }
      // Check before polling, not only after a dispatch: `finished` may already
      // be latched (see the pre-spawn check above), in which case no further
      // event will ever arrive to wake us.
      if (lease_obj->finished) {
        if (!h->revoked_.exchange(true, std::memory_order_acq_rel)) {
          // Per spec the causes are hot-unplug or the compositor losing DRM
          // master (every VT switch away). We cannot tell them apart from this
          // event alone; WS7 classifies and drives recovery. For now: latch
          // revoked so renderers stop committing against dead KMS objects.
          ihs::log::warn(
              "[leased-drm] lease revoked (wp_drm_lease_v1.finished) on "
              "connector '{}' (id {}) — commits gated",
              h->connector_name_, h->connector_id_);
        }
        return;
      }
      wl_display_flush(display);

      pollfd pfds[2] = {
          {wl_display_get_fd(display), POLLIN, 0},
          {stop_fd, POLLIN, 0},
      };
      const int r = poll(pfds, nfds, poll_timeout);
      if (r < 0) {
        if (errno == EINTR) {
          continue;
        }
        ihs::log::error("[leased-drm] monitor poll failed: {}",
                        std::strerror(errno));
        h->revoked_.store(true, std::memory_order_release);
        return;
      }
      if (r == 0) {
        continue;  // fallback tick — re-check `stop` / `finished`
      }
      if (stop_fd >= 0 && (pfds[1].revents & POLLIN) != 0) {
        return;  // orderly teardown
      }
      if ((pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) == 0) {
        continue;
      }
      if (wl_display_dispatch(display) < 0) {
        // Connection death: the compositor exited or crashed. The lessor fd is
        // gone, so the lease is already kernel-dead.
        ihs::log::error(
            "[leased-drm] Wayland connection lost — lease is revoked "
            "(compositor exit/crash)");
        h->revoked_.store(true, std::memory_order_release);
        return;
      }
    }
  });

  res.error = LeaseError::kNone;
  res.hold = std::move(hold);
  return res;
}

}  // namespace homescreen
