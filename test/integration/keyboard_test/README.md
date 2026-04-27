# keyboard_test

Integration test app for the ivi-homescreen keyboard pipeline.

## What it covers

The app drives both keyboard dispatch paths in the homescreen shell and
provides visual + machine-readable confirmation that each mode was
exercised:

1. **HardwareKeyboard / embedder API path** — every `KeyEvent`
   (`KeyDownEvent`, `KeyUpEvent`, `KeyRepeatEvent`) is logged with its
   `physicalKey`, `logicalKey`, `character`, and the active modifier set.
   This exercises `FlutterEngineSendKeyEvent` end-to-end.

2. **TextInputPlugin / `flutter/textinput` channel** — single-line and
   multi-line `TextField`s exercise text insertion (ASCII + non-ASCII),
   Backspace, Delete, cursor navigation including Up/Down across lines,
   Home/End, Shift-extended selection, single-line `TextInputAction`, and
   multi-line newline insertion.

3. **Coverage checklist** — every keyboard mode the test cares about lights
   up the first time it is observed:

   | Mode | Trigger |
   | --- | --- |
   | Key down / up / repeat | Press, release, hold any key |
   | Shift / Ctrl / Alt / Logo / Caps / Num | Press the modifier or toggle the lock |
   | Letter typed | Type a printable ASCII letter into a field |
   | Non-ASCII typed | Type a non-ASCII character (e.g. é, ö, 中) |
   | Backspace / Delete | Edit a field |
   | Left/Right arrow | Move the cursor |
   | Up/Down across lines | Move the cursor between lines in the multi-line field |
   | Home / End | Jump to start/end of line |
   | Page Up / Page Down | Press them |
   | Function key | F1..F24 |
   | Tab / Escape | Press them |
   | Enter action | Press Enter in the single-line field |
   | Multiline newline | Press Enter in the multi-line field |
   | Shift selection | Shift+Left/Right |
   | Ctrl shortcut | Ctrl+letter (e.g. Ctrl+A) |

## Building

```
cd test/keyboard_test
flutter pub get
flutter build bundle
```

The resulting `build/flutter_assets/` directory is the bundle to point
ivi-homescreen at via `--bundle=` (or `-b`).

## Running against ivi-homescreen

```
homescreen -b $(pwd)/build/flutter_assets
```

Then drive the keyboard manually, or via an automation tool (libinput,
ydotool, an HID injector, etc.).

## Self-check / CI

Tap the checklist icon in the AppBar (or whatever maps to it) to print a
JSON summary to stdout:

```
KEYBOARD_TEST_SUMMARY {"covered":[...],"missing":[...],"total":24,"coveredCount":19}
```

A CI runner can scrape that line, parse the JSON, and assert
`missing` is empty (or that specific modes are covered).

The `Reset` button in the AppBar clears the observed set so the run can
be repeated without restarting the app.

## Notes

- The app autofocuses a top-level `Focus` so `HardwareKeyboard` events
  reach it even before any `TextField` is focused. Tab / Escape / function
  keys won't necessarily land in a text field, but they still light up
  their checklist rows via the embedder API path.
- Modifier-only key presses (Shift, Ctrl, …) don't insert text but do
  light up the corresponding modifier rows.
- Coverage only goes up — there is no "uncover" event. Use Reset.
