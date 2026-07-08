# gesture_playground

An integration-test app for the ivi-homescreen embedder that exercises a set of
custom, map-style gesture recognizers built directly on Flutter's gesture arena,
plus a live diagnostic surface for the full pointer input stack. It is useful for
validating pointer input (touch, mouse, trackpad, stylus) on a given embedder /
backend.

## What it provides

`MapGestureDetector` (see `lib/map_gesture_detector/map_gesture_detector.dart`)
wraps a `RawGestureDetector` and combines these gestures:

| Gesture        | Recognizer                          | Callback signature                                            |
| -------------- | ----------------------------------- | ------------------------------------------------------------- |
| Tap            | `TapGestureRecognizer`              | `onTap`, `onTapDown`                                           |
| Long press     | `LongPressGestureRecognizer`        | `onLongPress`                                                 |
| Scroll / pan   | `ScrollGestureRecognizer`           | `onScroll(delta)`, `onScrollEnd(pos, vel)`                    |
| Pinch          | `PinchGestureRecognizer`            | `onPinch(center, scaleDelta)`                                 |
| Rotate         | `RotateGestureRecognizer`           | `onRotate(angleDelta)`                                        |
| Pitch / tilt   | `PitchGestureRecognizer`            | `onPitch(verticalDelta)`                                     |
| 3-finger swipe | `ThreeFingerSwipeGestureRecognizer` | `onThreeFingerSwipe(delta)`, `onThreeFingerSwipeEnd(pos, vel)` |

The tap and long-press recognizers are Flutter built-ins; the rest are custom.

### Design

The scroll, pinch, rotate, and pitch recognizers extend
`DoubleFingerGestureRecognizer`, which handles the gesture-arena bookkeeping for
one or two pointers and exposes `onUpdate` / `onMove` / `onUp` / `onCancel`
hooks. `ThreeFingerSwipeGestureRecognizer` tracks three simultaneous pointers
and drives them on `GestureRecognizer` directly (the two-finger base does not
generalize past two).

> **Touch, not trackpad — a deliberate scope decision.** These recognizers
> decode multi-touch gestures *manually in Flutter* from discrete touch pointers
> (`PointerDeviceKind.touch`), which is what a real multitouch **touchscreen**
> delivers. They intentionally do **not** target trackpads: on Linux, libinput
> (the common input source for both the DRM/KMS and Wayland backends) pre-cooks
> touchpad input into high-level scroll/pinch/swipe events and never exposes the
> individual fingers, so a trackpad's fingers can't reach these recognizers. A
> touchpad's two-finger scroll arrives as an ordinary `PointerScrollEvent`. The
> Flutter `PointerPanZoom` / trackpad path is not used, since the embedder is
> opinionated toward libinput's touch model rather than synthesizing pan/zoom.

Each recognizer drives a small state machine expressed as a Dart **`sealed`**
hierarchy — `PointerState`, `ScrollState`, `PinchState`, `RotateState`,
`PitchState`, `ThreeFingerState` — so state dispatch is checked for
exhaustiveness at compile time.

Activation is governed by per-gesture **slop** (a threshold the pointers must
cross before callbacks start), and `pitch` additionally has an angular
`tolerance`. `claimVictoryOnStart` controls whether a gesture takes exclusive
ownership of its pointers once it activates.

All recognizers accept **every** `PointerDeviceKind` (touch, mouse, trackpad,
stylus) by default; pass `supportedDevices` to `MapGestureDetector` (or an
individual recognizer) to restrict them.

### Diagnostic surface

`lib/mouse_test_frame.dart` (`MouseTestFrame`) is the app's home widget and a
live HUD over the pointer input stack. It reports, per frame:

- every active pointer's kind, pressure, tilt, and orientation (3+ tracked);
- hover position and a cursor shape that changes on hover / press
  (`MouseRegion` + `SystemMouseCursors`);
- the engine's raw coalesced sample count vs. framework move-event count, plus
  the last event timestamp and inter-move delta.

Input maps to gestures two ways:

- **left-button drag** drives the real `MapGestureDetector` recognizers;
- **right-button drag** simulates a two-finger gesture with one mouse —
  plain drag pans, `Shift` pinches, `Alt` tilts, `Ctrl` rotates.

A touchscreen or stylus drives the recognizers and HUD directly.

## Running

Locally with the Flutter tool:

```sh
flutter pub get
flutter test         # unit tests for every recognizer
flutter analyze
flutter run          # runs on the host (desktop) target
```

Requires Dart 3 (the state machines use sealed classes).

On the ivi-homescreen embedder, build a bundle with `emb` and point the
embedder at it:

```sh
emb bundle --app-path . --build --mode release
ivi-homescreen -b <workspace>/bundle/.-release-x86_64
```

The embedder auto-selects the Wayland-EGL backend when a Wayland session is
present; pass `--backend` / `--drm-device` for DRM/KMS or software backends.

## Not yet covered

Roughly in order of usefulness for embedder validation:

- Multi-view + device-pixel-ratio awareness: `MapGestureDetector` reads
  `physicalTouchSlop` from the first view only.
- Compare against Flutter's built-in `InteractiveViewer`, which covers a large
  subset of pan / zoom / rotate out of the box.
