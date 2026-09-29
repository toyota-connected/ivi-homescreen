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
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <system_error>
#include <utility>

#include "asio/io_context.hpp"
#include "asio/steady_timer.hpp"
#include "logging/logging.h"
#include "vsync/ivsync_provider.h"

/**
 * @brief Notice that a page-flip event was lost, and hand Flutter its baton
 *        back so frames resume.
 *
 * A parked baton means Flutter asked for a frame and is waiting for the flip
 * event of the commit in flight. If that event never arrives the baton is never
 * returned, no frame is built, and because no frame is built the present path
 * -- and so its own spin guard -- never runs again. The display stops until a
 * VT switch. That was #649; its causes are fixed, and this is the net for
 * whatever else loses an event (a driver dropping one, a session paused
 * mid-flip).
 *
 * Runs on the task runner's io_context, which is alive precisely when the
 * present path is not. Detection needs no timestamp: a tick that sees a baton
 * parked, the latch up, and the flip count unmoved since the previous tick --
 * which also saw it parked -- has watched a full tick pass with no progress.
 *
 * Shared by both DRM backends. The policy above is the same for both; what is
 * not is how each one clears its latch, which is the ClearLatch hook. Keeping
 * one implementation is not only about the duplicated lines: DRM-KMS-Vulkan
 * mirrors its latch into the provider and has to clear both, and when the
 * detector existed twice that was a thing to remember rather than a thing the
 * interface asked for (#660).
 *
 * Thread safety: NoteFlip() is called from the card's flip reader thread;
 * Tick() runs on the io_context. The count is atomic for that reason.
 * Everything else is touched only from the io_context (Start/Stop bracket the
 * timer's life).
 */
class StallDetector {
 public:
  /// Clear whichever latch the provider is gating on. Returns true if a latch
  /// was actually up -- false means something else is holding the baton, which
  /// is still worth handing back but is not a lost flip event.
  ///
  /// Every latch, not the first one. A backend that mirrors its latch elsewhere
  /// clears both here, or the baton it hands back is replaced by one that parks
  /// forever and the recovery is a no-op that logs like a success.
  using ClearLatch = std::function<bool()>;

  /// @param vsync    provider owning the baton
  /// @param paused   true while the session is revoked; such a tick is exempt
  /// @param clear    see ClearLatch
  /// @param tag      log prefix, e.g. "DrmBackend"
  StallDetector(ivi::IVsyncProvider& vsync,
                const std::atomic<bool>& paused,
                ClearLatch clear,
                const char* tag)
      : vsync_(vsync), paused_(paused), clear_(std::move(clear)), tag_(tag) {}

  StallDetector(const StallDetector&) = delete;
  StallDetector& operator=(const StallDetector&) = delete;

  /// A flip event was handled: the progress signal a tick looks for.
  ///
  /// Flip events, not commits. A commit counter advances during exactly the
  /// stall being detected -- the raster thread keeps committing into a pipeline
  /// whose events stopped -- and would read as progress.
  void NoteFlip() { flips_.fetch_add(1, std::memory_order_release); }

  /// Begin ticking on @p io. Safe to call again; the later call wins.
  void Start(asio::io_context& io) {
    timer_ = std::make_unique<asio::steady_timer>(io);
    Arm();
  }

  /// Stop ticking. Called before the provider is torn down: an async_wait left
  /// outstanding keeps the io_context's worker thread from finishing.
  void Stop() {
    if (timer_) {
      timer_->cancel();
    }
  }

  /// One tick. Public so a test can drive it without waiting out the threshold.
  void Tick() {
    // A revoked session is not a stall. The kernel sends no flip event for a
    // commit made before the revoke, and the latch stays up until resume clears
    // it -- so every VT switch-out longer than the threshold would read exactly
    // like a lost event. Forget what the last tick saw as well, or the first
    // tick after resume pairs with a pre-pause one.
    if (paused_.load(std::memory_order_acquire)) {
      last_parked_ = false;
      return;
    }

    // A baton parked by SetParked is a deliberate stop (a view whose output
    // went away), not a stall -- it has no flip to wait for, and unparking
    // delivers it.
    const bool parked = vsync_.HasParkedBaton() && !vsync_.IsParked();
    const uint64_t flips = flips_.load(std::memory_order_acquire);
    const bool no_progress = parked && last_parked_ && flips == last_flips_;
    last_parked_ = parked;
    last_flips_ = flips;
    if (!no_progress) {
      return;
    }

    if (!clear_()) {
      // Nothing was gating on a latch we own, so something else is holding the
      // baton. Hand it back anyway -- that is the point -- but do not claim a
      // flip was lost.
      (void)vsync_.DeliverParkedBaton();
      return;
    }

    ++recoveries_;
    if (!warned_) {
      warned_ = true;
      ihs::log::warn(
          "[{}] no page-flip event for a commit in flight and a baton waiting: "
          "cleared the latch and returned the baton so frames resume. A lost "
          "flip event, not a slow one. Further recoveries counted, not logged.",
          tag_);
    }
    (void)vsync_.DeliverParkedBaton();
  }

  /// Flip events handled. A test reads this to tell when the reader thread has
  /// dispatched a specific flip, and a tick reads it as the progress signal.
  [[nodiscard]] uint64_t flips() const {
    return flips_.load(std::memory_order_acquire);
  }
  /// Stalls recovered from.
  [[nodiscard]] uint64_t recoveries() const { return recoveries_; }

 private:
  /// IVI_VSYNC_STALL_MS overrides the threshold; 0 disables the detector. The
  /// default is well clear of any legitimate park: a baton waits one refresh
  /// period in steady state, and the longest legitimate wait measured on a
  /// loaded board was 127 ms, against the present path's own 100 ms guard.
  static int ThresholdMs() {
    static const int kMs = []() {
      const char* env = std::getenv("IVI_VSYNC_STALL_MS");
      if (env == nullptr) {
        return 1000;
      }
      char* end = nullptr;
      const long v = std::strtol(env, &end, 10);
      if (end == env || *end != '\0' || v < 0 || v > 60000) {
        ihs::log::warn(
            "[StallDetector] IVI_VSYNC_STALL_MS={} is not a value in "
            "[0, 60000]; using the 1000 ms default",
            env);
        return 1000;
      }
      return static_cast<int>(v);
    }();
    return kMs;
  }

  void Arm() {
    if (!timer_ || ThresholdMs() == 0) {
      return;
    }
    timer_->expires_after(std::chrono::milliseconds(ThresholdMs()));
    timer_->async_wait([this](const std::error_code& ec) {
      // The ec check must come first and touch nothing else. Destroying the
      // timer cancels outstanding waits, but their handlers still run on the
      // io_context afterwards -- by which time this detector may be gone. With
      // operation_aborted they return without dereferencing the capture, which
      // is what makes teardown without an explicit Stop() safe.
      if (ec) {
        return;  // canceled: teardown
      }
      Tick();
      Arm();
    });
  }

  ivi::IVsyncProvider& vsync_;
  const std::atomic<bool>& paused_;
  ClearLatch clear_;
  const char* tag_;

  std::unique_ptr<asio::steady_timer> timer_;
  std::atomic<uint64_t> flips_{0};
  uint64_t last_flips_{0};
  bool last_parked_{false};
  uint64_t recoveries_{0};
  bool warned_{false};
};
