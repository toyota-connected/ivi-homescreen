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

#include "vsync/ivsync_provider.h"

/**
 * @brief Arm a "flip in flight" latch before a nonblocking commit, and disarm
 *        it unless the commit was accepted.
 *
 * The latch must go up *before* the ioctl. A NONBLOCK commit's
 * PAGE_FLIP_EVENT can be read and dispatched by the card's flip reader thread
 * before the raster thread gets back from the ioctl, so a latch raised
 * afterwards re-arms for a flip that has already retired. Flutter's next
 * SubmitBaton then sees IsSourcePending() and parks the baton, no event is
 * coming to return it, and with no baton there is no frame -- so the
 * WaitForPendingFlip timeout inside Present never runs either. The display
 * stops until a VT switch drains it. That is #649.
 *
 * The software sink already gets this right (software/drm_dumb_sink.cc); this
 * exists so the other nine commit sites do too, without each one hand-rolling
 * the store and every one of its failure exits remembering to undo it.
 *
 * Usage: construct immediately before the commit, call Commit() when the
 * ioctl reports success, and let it go out of scope otherwise.
 *
 *   ScopedFlipArm arm(flip_pending_, vsync, nonblocking, drives_vsync, false);
 *   if (commit(...) != 0) {
 *     return Fallback();        // destructor disarms and drains
 *   }
 *   arm.Commit();
 *
 * Raster thread only. The latch is read by the flip reader thread, which is
 * why the stores are release.
 */
class ScopedFlipArm {
 public:
  /**
   * @param latch                 the backend's flip-in-flight latch
   * @param vsync                 provider owning the baton
   * @param nonblocking           false for a blocking modeset or cursor enable,
   *                              which gets no event and must not be armed
   * @param drives_vsync          false on a secondary output, whose failed
   *                              commit must not hand back the primary's baton
   * @param mirror_source_pending also drive IVsyncProvider::SetSourcePending,
   *                              which the Vulkan backend uses as its latch
   */
  ScopedFlipArm(std::atomic<bool>& latch,
                ivi::IVsyncProvider& vsync,
                const bool nonblocking,
                const bool drives_vsync,
                const bool mirror_source_pending)
      : latch_(latch),
        vsync_(vsync),
        armed_(nonblocking),
        drives_vsync_(drives_vsync),
        mirror_source_pending_(mirror_source_pending) {
    if (!armed_) {
      return;
    }
    latch_.store(true, std::memory_order_release);
    if (mirror_source_pending_) {
      vsync_.SetSourcePending(true);
    }
  }

  ~ScopedFlipArm() { Abort(); }

  ScopedFlipArm(const ScopedFlipArm&) = delete;
  ScopedFlipArm& operator=(const ScopedFlipArm&) = delete;
  ScopedFlipArm(ScopedFlipArm&&) = delete;
  ScopedFlipArm& operator=(ScopedFlipArm&&) = delete;

  /// The commit was accepted: the latch stays up for the flip reader to clear.
  /// Deliberately does not re-arm -- by now an early completion may already
  /// have cleared it, and raising it again is the bug this class exists for.
  void Commit() noexcept { armed_ = false; }

  /// Disarm now, before a fallback or retry rather than at scope exit. The
  /// order matters: clear the latch first, then drain. A SubmitBaton racing in
  /// ahead of the store parks and is drained here; one arriving after it sees
  /// the latch clear and drains itself. Either way the baton moves.
  void Abort() noexcept {
    if (!armed_) {
      return;
    }
    armed_ = false;
    latch_.store(false, std::memory_order_release);
    if (mirror_source_pending_) {
      vsync_.SetSourcePending(false);
    }
    if (drives_vsync_) {
      (void)vsync_.DeliverParkedBaton();
    }
  }

  /// True while this object still owns an arm that has not been committed or
  /// aborted. For assertions and tests.
  [[nodiscard]] bool armed() const noexcept { return armed_; }

 private:
  std::atomic<bool>& latch_;
  ivi::IVsyncProvider& vsync_;
  bool armed_;
  const bool drives_vsync_;
  const bool mirror_source_pending_;
};
