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
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

// drm::input::Seat (input events + device opening + VT suspend/resume) and the
// KeyboardEvent / KeyboardLeds data types stay; keyboard translation + repeat
// now come from the backend-shared input core below.
#include <drm-cxx/input/keyboard.hpp>
#include <drm-cxx/input/seat.hpp>

#include <shell/platform/embedder/embedder.h>

#include "input/iseat.h"
#include "input/key_repeater.h"
#include "input/xkb_keyboard.h"

namespace homescreen {

class DrmCursor;

// Owned-string variant of drm::input::KeymapOptions, safe to pass
// across threads. drm::input::KeymapOptions itself uses string_views,
// which can't survive cross-thread hand-off.
struct KeymapConfig {
  std::string rules;
  std::string model;
  std::string layout;
  std::string variant;
  std::string options;
};

// libinput-backed seat. Pumps drm::input::Seat events on a dedicated thread
// and translates them into FlutterPointerEvent / FlutterKeyEvent. Events
// received before the Flutter engine is up are dropped (the state pointer
// resolves to a null engine handle in that window).
//
// When a libseat-backed session is live, the caller passes an
// InputDeviceOpener (from DrmSession::InputOpener()) so libinput's
// privileged /dev/input/event* opens route through libseat — giving
// input fds the same revocable lifetime as the DRM fd. In that mode,
// the K_OFF TTY keyboard-mode hack is skipped; logind/seatd manages
// VT keyboard state on session activation.
class DrmSeat final : public ISeat {
 public:
  DrmSeat(int32_t viewport_width,
          int32_t viewport_height,
          drm::input::InputDeviceOpener opener = {});
  ~DrmSeat() override;

  bool Start() override;
  void Stop() override;
  void SetViewControllerState(
      FlutterDesktopViewControllerState* state) override;

  // Set (or clear) the DRM hardware cursor this seat moves. Safe to
  // call from any thread; the dispatch loop reads via acquire and
  // ignores nullptr. Caller must clear (pass nullptr) before the
  // pointed-to DrmCursor is destroyed.
  void SetCursor(DrmCursor* cursor) {
    cursor_.store(cursor, std::memory_order_release);
  }

  // Hot-swap the xkb keymap. Safe to call from any thread; the actual
  // reload runs on the dispatch thread on the next poll iteration.
  // Held keys and lock latches are preserved across the swap. On
  // failure (bad RMLVO names), the existing keymap is left intact and
  // a warning is logged. Empty fields fall back to xkb's system
  // defaults for that knob. Currently dormant — no caller wires this;
  // intended for a future settings/locale channel.
  void ReloadKeymap(KeymapConfig cfg);

  // Handler invoked on a Ctrl+Alt+F<n> chord. With K_OFF on the VT,
  // the kernel's keymap-layer chord handler doesn't run — DrmSeat
  // intercepts the combination from libinput's stream and asks the
  // installed handler to perform the session switch (typically via
  // libseat). Receives the VT number (F1=1, …, F12=12). Returning
  // true consumes the event so it doesn't also reach Flutter; false
  // means the handler couldn't switch and the keys should still flow
  // through. Set from any thread before Start(); the dispatch thread
  // reads via acquire.
  using VtSwitchHandler = std::function<bool(int)>;
  void SetVtSwitchHandler(VtSwitchHandler handler);

  // Session lifecycle hooks, called from the libseat dispatch thread
  // (NOT this seat's dispatch thread). Flip the pending flag here;
  // DispatchLoop picks it up on its next iteration and calls the
  // matching libinput suspend/resume on the right thread. Without
  // this dance, after a VT round-trip libinput's evdev fds are stale
  // (revoked during pause) and key events stop flowing — the symptom
  // is "chord works once, then never again."
  void OnSessionPaused();
  void OnSessionResumed();

  // Update the cursor clamping rectangle. Called by FlutterView after
  // DrmBackend::Create resolves the actual framebuffer dimensions
  // (which can differ from the config's view.width/height when `-f`
  // promoted the FB to the full mode). Must be called before Start()
  // so the dispatch thread sees the final values without atomics; the
  // dispatch thread's pointer math reads these once on the hot path.
  void SetViewport(int32_t width, int32_t height);

  // Scanout rotation in degrees (0|90|180|270). The pointer accumulates in
  // render (viewport) space — where Flutter hit-tests, so events stay correct
  // — but the HW cursor plane lives in panel space, so FlushCursorMotion
  // rotates the position by this amount before placing the sprite. Touch is
  // unaffected (the digitizer already tracks the panel). Set before Start().
  void SetCursorRotation(int32_t degrees) { cursor_rotation_ = degrees; }

  // Per-device relative-pointer transforms. Each spec is
  // "<device-name-substring>=<rot>[,flip-x][,flip-y]" (rot = 0|90|180|270). A
  // pointer bolted to a rotated chassis (e.g. the Steam Deck's right trackpad)
  // emits deltas in the panel's physical frame; this rotates/reflects them to
  // match the rotated display, leaving unmatched devices (an external mouse)
  // alone. Set before Start().
  void SetInputTransforms(const std::vector<std::string>& specs);

 private:
  void DispatchLoop();
  void ApplyPendingKeymap();
  // If pointer motion accumulated during the previous dispatch batch,
  // issue a single cursor commit with the latest position. Cursor
  // commits are blocking atomic flips (one vblank each on amdgpu DC),
  // so coalescing per batch keeps the seat thread from queueing one
  // commit per libinput event when the mouse outpaces vblank.
  void FlushCursorMotion();
  void HandleEvent(const drm::input::InputEvent& ev);
  void HandleKeyboard(const drm::input::KeyboardEvent& ev);
  void DispatchKeyToFlutter(const drm::input::KeyboardEvent& resolved) const;
  // Push the keyboard's current Caps/Num/Scroll latch to the physical LEDs.
  // The Seat only re-applies LEDs on device-add, so this must be called at
  // startup and whenever a lock key toggles.
  void SyncKeyboardLeds();
  void HandlePointerMotion(const drm::input::PointerMotionEvent& ev);
  void HandlePointerButton(const drm::input::PointerButtonEvent& ev);
  void HandlePointerAxis(const drm::input::PointerAxisEvent& ev) const;
  void HandleTouch(const drm::input::TouchEvent& ev) const;

  [[nodiscard]] FLUTTER_API_SYMBOL(FlutterEngine) CurrentEngine() const;

  // Mutable so FlutterView can rewrite them in SetViewport after
  // DrmBackend::Create has resolved the actual framebuffer size
  // (e.g. when `-f` promoted the FB to the full mode). Read on the
  // dispatch thread; SetViewport runs on the main thread before
  // Start() is called, so no atomic needed.
  int32_t viewport_w_;
  int32_t viewport_h_;
  // Scanout rotation (0|90|180|270) applied to the cursor sprite position only.
  int32_t cursor_rotation_ = 0;
  // Per-device relative-pointer delta transforms (see SetInputTransforms).
  // Matched by device-name substring in HandlePointerMotion; first match wins.
  struct PointerTransform {
    std::string match;
    int rotation = 0;  // 0|90|180|270 applied to (dx, dy)
    bool flip_x = false;
    bool flip_y = false;
  };
  std::vector<PointerTransform> pointer_transforms_;
  // Last transformed render position per touch slot, so a touch-UP (which
  // libinput delivers with no coordinates) reuses it instead of snapping to a
  // corner. Touched only from the seat dispatch thread (HandleTouch is const).
  mutable std::unordered_map<int32_t, std::pair<double, double>> touch_pos_;

  std::atomic<FlutterDesktopViewControllerState*> state_{nullptr};

  // Caller-supplied hook for libinput's privileged device opens. Empty
  // when there's no libseat session (falls back to ::open/::close inside
  // drm::input::Seat).
  drm::input::InputDeviceOpener opener_;

  std::unique_ptr<drm::input::Seat> seat_;
  // Backend-shared keyboard translation + auto-repeat (shell/input/), the same
  // path SoftwareSeat uses. repeater_ is null when disabled via
  // IVI_DRM_KEY_REPEAT=0 or when its timerfd can't be created.
  std::unique_ptr<input::XkbKeyboard> keyboard_;
  std::unique_ptr<input::KeyRepeater> repeater_;
  std::thread thread_;
  std::atomic<bool> stop_{false};

  // Keymap reloads posted from any thread, drained on the dispatch
  // thread by ApplyPendingKeymap() at the top of each DispatchLoop
  // iteration. Owns its strings so caller threads don't have to keep
  // them alive past ReloadKeymap() returning.
  std::mutex pending_mu_;
  std::optional<KeymapConfig> pending_keymap_;

  // Ctrl+Alt+F<n> chord handler. Written via SetVtSwitchHandler (any
  // thread, before Start()); read by the dispatch thread inside
  // HandleKeyboard. The mutex guards installation; the handler itself
  // is called without the mutex held (it does its own libseat work
  // and may block briefly).
  std::mutex vt_switch_mu_;
  VtSwitchHandler vt_switch_handler_;

  // Pending libinput suspend/resume action posted from the libseat
  // dispatch thread (via On*Paused/Resumed) and applied on this
  // seat's dispatch thread at the top of DispatchLoop. libinput is
  // single-threaded, so the suspend/resume call MUST happen on the
  // thread that owns the libinput context.
  enum class PendingSessionAction : uint8_t { kNone, kSuspend, kResume };
  std::atomic<PendingSessionAction> pending_session_action_{
      PendingSessionAction::kNone};

  // VT keyboard mode. In a text console, without K_OFF keystrokes are
  // also consumed by the kernel tty line discipline (echoed to the
  // underlying shell). K_OFF suppresses that; xkbcommon translation of
  // libinput events is unaffected and happens via keyboard_.
  int tty_fd_ = -1;
  int saved_kb_mode_ = 0;

  // Accessed only from the dispatch thread.
  double pointer_x_ = 0.0;
  double pointer_y_ = 0.0;
  int64_t button_mask_ = 0;
  bool pointer_added_ = false;
  // Set by HandlePointerMotion, cleared by FlushCursorMotion. Lets the
  // dispatch loop coalesce a batch of motion events into one cursor
  // commit at the end of seat_->dispatch().
  bool cursor_motion_pending_ = false;

  // KMS hardware cursor target. Owned by DrmBackend; this seat just
  // forwards motion to it. Atomic because the wiring runs on a
  // different thread than the dispatch loop that reads it.
  std::atomic<DrmCursor*> cursor_{nullptr};
};

}  // namespace homescreen
