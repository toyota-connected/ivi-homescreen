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

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace homescreen {

// drm-lease-v1 client for the wayland-leased-drm backend family.
//
// The Wayland connection here is used ONLY to negotiate a DRM lease and to
// watch for its revocation — this client never creates a wl_surface and takes
// no input from the compositor (evdev does that). Negotiation is synchronous
// with a deadline on the caller's thread; once granted, the wl_display moves to
// a monitor thread that lives for the process lifetime so `finished` and
// `withdrawn` are always observed.

// A connector the compositor has advertised as available for leasing.
struct LeaseOffer {
  std::string name;  // e.g. "HDMI-A-1"; stable within a compositor session
  std::string description;  // human-readable, from EDID
  uint32_t connector_id{};  // kernel mode-object id; stable across compositor
                            // restarts on the same boot
};

// Why a lease ended. Distinct because they demand different operator actions:
// master-loss is routine and self-healing on VT return, a
// hot-unplug needs someone to plug the panel back in, and a dead compositor is
// a supervision problem.
enum class RevokeCause {
  // wp_drm_lease_v1.finished. The spec's causes are the compositor losing DRM
  // master — i.e. every VT switch away, the common case — or a hot-unplug. The
  // protocol carries no way to tell them apart; the connector's `withdrawn`
  // event hints at unplug but is not sent for master-loss.
  kFinished,
  // The wl_display died: the compositor exited or crashed. The lessor's fd went
  // with it, so the lease is already kernel-dead.
  kConnectionLost,
  // The monitor thread itself failed (poll error). Not a protocol event; the
  // lease's true state is unknown, so it is treated as lost.
  kMonitorFailure,
};

const char* ToString(RevokeCause c);

struct LeaseConfig {
  // Selects the wp_drm_lease_device_v1 global when several are advertised (one
  // per DRM node). Either a decimal index into the advertised order, or a DRM
  // node path ("/dev/dri/card1") matched against the path derived from each
  // device's enumeration drm_fd. Unset = the sole device; ambiguous + unset is
  // a hard error.
  std::optional<std::string> device;

  // Connector to request, by name. Unset + exactly one offer = take it;
  // unset + several offers is a hard error that lists what was offered.
  std::optional<std::string> connector;

  // Wall-clock bound on the whole negotiation. Load-bearing: the spec lets a
  // compositor defer `drm_fd` until it regains DRM master, so without a
  // deadline a VT-switched-away compositor would hang startup indefinitely.
  uint32_t timeout_ms{5000};

  // Called exactly once, when the lease is first observed revoked. Runs on the
  // monitor thread — or inline on the caller's thread from Acquire() itself, if
  // `finished` arrived in the same dispatch batch as `lease_fd`. It must be
  // cheap and must not block: the monitor thread is what teardown joins.
  //
  // Passed here rather than set on the LeaseHold afterwards because Acquire()
  // spawns the monitor before it returns — a setter would race the very first
  // revocation, which is precisely the case worth handling.
  //
  // Null = latch revoked() and nothing else. The renderers' gates still fire,
  // so the display stops updating and stays that way. That is a deliberate
  // choice
  // (`gate`), not a default to fall into: see the exit policy below.
  std::function<void(RevokeCause)> on_revoked;
};

// Why a negotiation attempt ended without a lease. Distinct causes because they
// demand different operator actions.
enum class LeaseError {
  kNone,  // no error — see Result::error for what this means per call
  kNoWaylandSession,    // WAYLAND_DISPLAY unset, or the socket doesn't exist
  kConnectFailed,       // socket exists but wl_display_connect failed
  kNoLeaseDevice,       // compositor does not implement drm-lease-v1
  kDeviceAmbiguous,     // several devices advertised, none selected
  kDeviceNotFound,      // the configured device index/path matched nothing
  kNoOffers,            // device enumerated, but advertised zero connectors
  kConnectorAmbiguous,  // several offers, none selected
  kConnectorNotFound,   // the configured connector name matched no offer
  kDenied,              // compositor sent `finished` instead of `lease_fd`
  kTimeout,             // deadline hit before a terminal event
  kProtocolError,       // wl_display error / EPIPE during negotiation
};

const char* ToString(LeaseError e);

// A granted lease: the master fd plus the live Wayland connection that backs
// it. Owning this object is what keeps the lease alive — destroying it sends
// wp_drm_lease_v1.destroy, letting the compositor revoke and re-advertise the
// connector.
//
// The monitor thread must outlive every backend built on the lease fd, so the
// display in every family owns the LeaseHold.
class LeaseHold {
 public:
  ~LeaseHold();

  LeaseHold(const LeaseHold&) = delete;
  LeaseHold& operator=(const LeaseHold&) = delete;

  // DRM master fd over exactly the leased object set. The kernel filters this
  // fd's resource view, so drmModeGetResources() on it returns only leased
  // objects. Stays valid (as a render fd) even after revocation.
  [[nodiscard]] int fd() const noexcept { return lease_fd_; }

  [[nodiscard]] uint32_t connector_id() const noexcept { return connector_id_; }
  [[nodiscard]] const std::string& connector_name() const noexcept {
    return connector_name_;
  }
  // DRM node path derived from the lease fd, for logging and OutputProvider.
  [[nodiscard]] const std::string& card_path() const noexcept {
    return card_path_;
  }

  // Set by the monitor thread on wp_drm_lease_v1.finished (or connection
  // death). Once true it never clears: reacquire builds a NEW LeaseHold.
  // Renderers gate their commits on this — a revoked fd's KMS objects are gone
  // and every ioctl naming them fails.
  [[nodiscard]] bool revoked() const noexcept {
    return revoked_.load(std::memory_order_acquire);
  }

 private:
  friend class LeaseClient;
  struct Impl;

  LeaseHold();

  // Latch revoked() and, the first time only, log the cause and run the
  // LeaseConfig::on_revoked handler. Idempotent and safe from either the
  // monitor thread or Acquire()'s own thread; every path that discovers a dead
  // lease goes through here so the "once" and the logging cannot drift apart.
  void LatchRevoked(RevokeCause cause);

  std::unique_ptr<Impl> impl_;
  int lease_fd_{-1};
  uint32_t connector_id_{};
  std::string connector_name_;
  std::string card_path_;
  std::atomic<bool> revoked_{false};
};

class LeaseClient {
 public:
  struct Result {
    // Acquire(): non-null iff the lease was granted. Probe() never sets it —
    // it does not negotiate, so use `error == kNone` to test a probe's success.
    std::unique_ptr<LeaseHold> hold;
    LeaseError error{};              // kNone = success
    std::vector<LeaseOffer> offers;  // what was advertised, for diagnostics
  };

  // Negotiate a lease. Blocks on the caller's thread up to cfg.timeout_ms.
  // On success spawns the monitor thread and hands back the hold, with
  // error == kNone.
  static Result Acquire(const LeaseConfig& cfg);

  // Read-only enumeration: bind the lease devices, take the drm_fd, collect the
  // connector offers, then release. Stops before create_lease_request, so it is
  // safe to run against a live desktop session. Backs the lease-aware mode
  // listing and `finished`-cause diagnostics.
  //
  // Returns error == kNone with `offers` populated on success (hold is always
  // null). A probe that reaches a compositor advertising nothing returns
  // kNoOffers.
  static Result Probe(const LeaseConfig& cfg);
};

// --lease-list-connectors handler. Scans @p argv; if the flag is absent returns
// -1 and the caller carries on. Otherwise probes the compositor, prints what it
// offers, and returns a process exit code.
//
// Takes raw argv rather than a Configuration because it must run before config
// parsing: this is the tool you reach for when a leased backend will not start,
// and it cannot require the bundle that the failing launch was trying to load.
// It reads --lease-device and --lease-timeout-ms off argv for the same reason.
//
// Exit codes are meant to be scriptable, so "reached the compositor and it
// offers nothing" is distinct from "no compositor" — on a wlroots host the
// former is the normal outcome and the interesting one:
//   0 — offers listed
//   1 — reached a lease device, but zero connectors are offered
//   2 — no Wayland session, or the compositor does not implement drm-lease-v1
int MaybeListLeaseConnectors(int argc, char** argv);

}  // namespace homescreen
