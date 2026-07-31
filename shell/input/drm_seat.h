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

#include "input/cursor_position_sink.h"
#include "input/iseat.h"
#include "input/key_repeater.h"
#include "input/wake_event_fd.h"
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

  // Set the DRM hardware cursor for the view under construction (the region
  // committed by the next SetViewControllerState). Call before Start(); region
  // registration is single-threaded during view construction, which completes
  // before the dispatch thread starts.
  void SetCursor(DrmCursor* cursor) { pending_.cursor = cursor; }

  // Set the composited cursor sink (the EGL backend's GlCursor) for the view
  // under construction, used when no hardware cursor plane is available. Call
  // before Start(). The sink receives un-rotated render/viewport coordinates
  // (it composites into the render target; scanout rotation is applied to the
  // whole framebuffer downstream).
  void SetGlCursor(ICursorPositionSink* sink) { pending_.gl_cursor = sink; }

  // Position of the view under construction in the combined pointer space
  // (from [view.output] x/y). Multiple views sharing one seat (several
  // displays on one DRM card) each declare where their render surface sits so
  // the pointer moves between them as laid out. Defaults to (0,0). Call before
  // Start(); committed by the next SetViewControllerState.
  void SetRegionLayout(int32_t x, int32_t y);

  // Bind a touch panel (libinput device-name substring) to the view under
  // construction, so its touches route to this view instead of the primary.
  // Call before Start(); committed by the next SetViewControllerState.
  void SetRegionTouchDevice(std::string name);

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

  // Set the render extent of the view under construction. Called after
  // DrmBackend::Create resolves the actual framebuffer dimensions (which can
  // differ from the config's view.width/height when `-f` promoted the FB to
  // the full mode). Sizes the region's rectangle in combined pointer space.
  // Must be called before Start() (region registration is single-threaded and
  // completes before the dispatch thread starts); the pointer math reads it on
  // the hot path.
  void SetViewport(int32_t width, int32_t height);

  // Scanout rotation in degrees (0|90|180|270). The pointer accumulates in
  // render (viewport) space — where Flutter hit-tests, so events stay correct
  // — but the HW cursor plane lives in panel space, so FlushCursorMotion
  // rotates the position by this amount before placing the sprite. Touch is
  // unaffected (the digitizer already tracks the panel). Set before Start().
  void SetCursorRotation(int32_t degrees) {
    pending_.cursor_rotation = degrees;
  }

  // Per-device relative-pointer transforms. Each spec is
  // "<device-name-substring>=<rot>[,flip-x][,flip-y]" (rot = 0|90|180|270). A
  // pointer bolted to a rotated chassis (e.g. the Steam Deck's right trackpad)
  // emits deltas in the panel's physical frame; this rotates/reflects them to
  // match the rotated display, leaving unmatched devices (an external mouse)
  // alone. Set before Start().
  void SetInputTransforms(const std::vector<std::string>& specs);

 private:
  // A rendered view occupying a rectangle of the combined pointer space. One
  // per Flutter view sharing this seat (multiple displays on one DRM card).
  // The pointer accumulates in combined space; the region under it receives
  // events in its own local (render) coordinates and owns the sprite that
  // tracks it. pointer_added tracks whether this view's engine has seen the
  // pointer device yet -- each engine needs its own kAdd before kMove/kHover.
  struct ViewRegion {
    FlutterDesktopViewControllerState* state = nullptr;
    int32_t layout_x = 0;  // top-left in combined pointer space
    int32_t layout_y = 0;
    int32_t width = 0;            // render/viewport extent
    int32_t height = 0;           // render/viewport extent
    int32_t cursor_rotation = 0;  // scanout rotation for the sprite (0|90|…)
    DrmCursor* cursor = nullptr;  // HW cursor plane (this CRTC)
    ICursorPositionSink* gl_cursor = nullptr;  // composited fallback
    bool pointer_added = false;
    // libinput device-name substring of the touch panel bonded to this view's
    // display; empty when none is configured. Touch is absolute (no cursor), so
    // it routes by device, not by pointer position.
    std::string touch_match;
  };

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

  // Resolve which region the (global) pointer is over after a motion delta,
  // clamping it back into the region set if the delta pushed it into a gap or
  // past the outer edge. Updates active_region_ and pointer_x_/pointer_y_.
  // No-op when regions_ is empty.
  void ResolveActiveRegion();
  // True when two regions are views of the same Flutter engine (one engine
  // spanning multiple displays). A drag may cross freely between such regions
  // as one continuous gesture; a drag between regions of DIFFERENT engines is
  // confined to the origin engine (engine isolation).
  [[nodiscard]] bool RegionsShareEngine(size_t a, size_t b) const;
  // Marshal a batch of pointer events to a region's engine on its platform
  // strand. No-op if the region has no live engine yet. Static: it works
  // entirely off the region and the events, touching no seat state.
  static void DispatchToRegion(const ViewRegion& region,
                               const FlutterPointerEvent* events,
                               size_t count);

  // Committed view regions, built by the per-view Set* setters before Start()
  // and read-only on the dispatch thread thereafter (registration is
  // single-threaded during view construction, which completes before
  // StartEvents() spawns the dispatch thread). Index 0 is the primary/first
  // view. A single-view seat has exactly one region at (0,0), so behavior is
  // unchanged from before the multi-region rework.
  std::vector<ViewRegion> regions_;
  // The region currently under the pointer; index into regions_. Updated by
  // HandlePointerMotion, read by the button/axis/cursor/keyboard paths.
  size_t active_region_ = 0;
  // Implicit pointer grab: on the first button-down the view under the pointer
  // captures the pointer until every button releases, so a drag keeps routing
  // to that view even if the cursor wanders onto another display. The cursor
  // itself still follows the hand across displays (no clamp), so releasing the
  // button never snaps the pointer -- it is already where the hand is.
  bool grab_active_ = false;
  size_t grab_region_ = 0;
  // The region whose HW cursor sprite is currently shown. When the active
  // region changes, FlushCursorMotion hides this one's sprite and shows the
  // new one's, so exactly one cursor is visible across the displays. Starts at
  // kNoRegion (nothing shown yet).
  static constexpr size_t kNoRegion = static_cast<size_t>(-1);
  // [[maybe_unused]]: only read/written under HAVE_DRM_CURSOR (the sprite
  // hide/show path); a build of drm-cxx without the cursor leaves it dormant.
  [[maybe_unused]] size_t cursor_shown_region_ = kNoRegion;
  // Region under construction: the per-view setters (SetViewport, SetCursor,
  // SetGlCursor, SetCursorRotation, SetRegionLayout) accumulate here, and
  // SetViewControllerState commits it into regions_ and resets it for the next
  // view.
  ViewRegion pending_;
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

  // Touch events accumulated since the last TOUCH_FRAME marker, and the
  // region they route to. libinput groups the per-slot updates of one
  // hardware scan between frame markers; dispatching the group as one batch
  // means one strand task and one FlutterEngineSendPointerEvent (= one
  // UI-thread task post in the engine) per scan, instead of one per contact
  // — a 10-finger drag at a 250 Hz scan rate is 250 posts/s instead of
  // 2500/s. Touched only from the seat dispatch thread.
  mutable std::vector<FlutterPointerEvent> touch_batch_;
  mutable const ViewRegion* touch_batch_region_ = nullptr;
  // The stamp shared by every contact in the open batch, latched from the
  // first one. See HandleTouch.
  mutable size_t touch_batch_ts_ = 0;
  void FlushTouchBatch() const;

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

  // Breaks DispatchLoop out of its infinite poll() when a cross-thread flag is
  // set: stop_ (Stop), a session pause/resume action (On*Paused/Resumed, posted
  // from the libseat dispatch thread), or a keymap reload (ReloadKeymap). Each
  // producer stores its flag then Wake()s; the loop drains and re-checks the
  // flags at the top. Without the session/keymap wakes, a VT switch-out under
  // poll(-1) would stall until the next input event (revoked-fd hang).
  input::WakeEventFd waker_;

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

  // Accessed only from the dispatch thread. pointer_x_/pointer_y_ are in
  // combined (global) pointer space -- ResolveActiveRegion maps them to a
  // region and its local render coordinates.
  double pointer_x_ = 0.0;
  double pointer_y_ = 0.0;
  int64_t button_mask_ = 0;
  // Set by HandlePointerMotion, cleared by FlushCursorMotion. Lets the
  // dispatch loop coalesce a batch of motion events into one cursor
  // commit at the end of seat_->dispatch().
  bool cursor_motion_pending_ = false;
};

}  // namespace homescreen
