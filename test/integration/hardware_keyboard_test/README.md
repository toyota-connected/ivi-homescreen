# hardware_keyboard_test

Integration test app for `services.HardwareKeyboard` against the
ivi-homescreen embedder.

This is a **pure HardwareKeyboard test** — it deliberately avoids every
piece of the legacy keyboard plumbing:

* No `TextField` / `TextInputPlugin` (no `flutter/textinput` channel).
* No `RawKeyboard` / `RawKeyEvent` (no `flutter/keyevent` channel).
* No `Shortcuts` / `Actions` / focus traversal.

The only contract under test is `FlutterEngineSendKeyEvent` landing as
`KeyEvent` objects on `HardwareKeyboard.instance`. That covers the modern
embedder-API path end-to-end.

## What it covers

| Surface | Mode rows |
| --- | --- |
| Event types | `KeyDownEvent`, `KeyUpEvent`, `KeyRepeatEvent` |
| Synthesized vs real | `event.synthesized` true / false |
| Modifier getters | `isShiftPressed`, `isControlPressed`, `isAltPressed`, `isMetaPressed` |
| Lock modes | `lockModesEnabled` ⊇ {`capsLock`, `numLock`, `scrollLock`} |
| Pressed sets | `physicalKeysPressed`, `logicalKeysPressed`, multi-key chord (≥2 held) |
| Character delivery | `event.character` non-null, non-ASCII codepoint |
| Logical key categories | letter, digit, arrow, nav, F1..F24, Tab, Esc, Enter, Space, Backspace |
| Modifier keys as events | `KeyEvent.logicalKey` is itself a modifier |
| Programmatic API | `syncKeyboardState()` button, `clearState()` button |

## Building

```
cd test/integration/hardware_keyboard_test
flutter pub get
flutter build bundle
```

`build/flutter_assets/` is the bundle to point ivi-homescreen at via `-b`.

## Running against ivi-homescreen

```
homescreen -b $(pwd)/build/flutter_assets
```

Drive the keyboard manually, or via `libinput` / `ydotool` / an HID
injector / etc.

## Self-check / CI

Tap the checklist icon (or `Icons.fact_check_outlined` in the AppBar) to
print a JSON summary line to stdout:

```
HARDWARE_KEYBOARD_TEST_SUMMARY {"covered":[...],"missing":[...],"total":29,"coveredCount":24}
```

A CI runner can scrape that line and assert `missing` is empty (or that
specific modes are covered).

The `Reset` button clears the observed set so the run can be repeated
without restarting the app.

## AppBar buttons

| Icon | Action |
| --- | --- |
| `Icons.sync` | calls `HardwareKeyboard.instance.syncKeyboardState()` |
| `Icons.layers_clear` | calls `HardwareKeyboard.instance.clearState()` |
| `Icons.refresh` | resets coverage |
| `Icons.fact_check_outlined` | prints the JSON summary line |

## Notes

* The body is a top-level `Focus` with `autofocus: true` and an
  `onKeyEvent` that returns `KeyEventResult.ignored`. That gives the
  embedder a focus target so it routes key events to the engine, without
  introducing a Shortcuts/Actions layer that would skew the test.
* A `KeyRepeatEvent` row only lights up if the embedder actually
  generates repeat events — most platforms do, but on a setup that maps
  hold-to-repeat to discrete down/up pairs this row will stay dark.
* `synthesizedEvent` typically lights up after `syncKeyboardState()` if
  the OS-reported state diverged from Flutter's, or after `clearState()`
  followed by another real key.
* `multiKeyChord` requires holding two physical keys simultaneously.
  Modifier + letter (e.g. `Ctrl+A` held) is the easiest way.