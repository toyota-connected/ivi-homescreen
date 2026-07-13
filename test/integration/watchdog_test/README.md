# watchdog_test

Integration test app for the ivi-homescreen `watchdog` platform channel.

Exercises the `MethodChannel('watchdog')` interface exposed by
`WatchdogPlugin` (built with `-DBUILD_WATCHDOG=ON`). No hardware interaction is
required — all checks are driven programmatically on startup and results are
shown in the UI.

## Checks

Checks run in the order listed. The first check (`main_thread_alive`) always
runs even when the channel is absent.

| Name | Description |
|---|---|
| `main_thread_alive` | Waits 5.5 s; if the process is still alive, the main-thread watchdog (default 5 s timeout) did not fire |
| `channel_available` | Channel is reachable (no `MissingPluginException`) |
| `start_stop` | `start({source:3})` → `stop({source:3})` succeed |
| `pet_active` | `start(3)` → `pet(3)` → `stop(3)` succeed |
| `pet_inactive_no_crash` | `pet(99)` on unregistered source does not throw |
| `stop_inactive_no_crash` | `stop(99)` on unregistered source does not throw |
| `multi_source` | Start / pet / stop on two sources (3 and 4) concurrently |
| `large_source_id` | `start/pet/stop(1000000)` — verifies no upper-bound restriction on source IDs |
| `invalid_source_neg` | `start(-1)` returns `PlatformException(invalid_source)` |
| `get_callbacks_shape` | `get_callbacks` returns a `Map` with non-zero `int` values for `start`, `pet`, `stop` |
| `ffi_start_pet_stop` | Casts the pointers via `dart:ffi`, calls `start(5)` / `pet(5)` / `stop(5)` — no crash |
| `start_with_name` | `start({source:6, name:'TestSource'})` → `pet(6)` → `stop(6)` — verifies the optional `name` argument is accepted |

Source IDs used in the tests (3, 4, 5, 99, 1,000,000) are all outside the
embedder-reserved range (0–2). There is no upper-bound restriction on source
IDs — only negative values are rejected.

### Skip behaviour

If `channel_available` detects the channel is absent (`MissingPluginException`
or an `unhandled_method` `PlatformException`), all remaining checks are marked
**skip** and the process exits with `WATCHDOG_TEST: SKIP`. This lets the
app run cleanly against a binary built without `-DBUILD_WATCHDOG=ON`.

## Building

### Prerequisites

```bash
# The embedder must be built with watchdog support for PASS results
cmake -DBUILD_WATCHDOG=ON ...
ninja -j$(nproc)
```

### Flutter app

```bash
cd test/integration/watchdog_test
flutter pub get
flutter build bundle
```

The bundle directory is `build/flutter_assets/`.

## Running

### Against a watchdog-enabled embedder

```bash
homescreen -b $(pwd)/build/flutter_assets
```

All 12 checks should light up green and stdout should show:

```
WATCHDOG_TEST_SUMMARY {"pass":12,"fail":0,"skip":0,...}
WATCHDOG_TEST: PASS
```

Note: the `main_thread_alive` check takes 5.5 s to complete, so the full run
takes at least that long.

### Against a non-watchdog build

Run the same command against an embedder built without `-DBUILD_WATCHDOG=ON`.
The `main_thread_alive` check still passes (the process is alive regardless),
then `channel_available` detects the missing channel and all remaining checks
are skipped:

```
WATCHDOG_TEST_SUMMARY {"pass":1,"fail":0,"skip":11,...}
WATCHDOG_TEST: SKIP
```

## CI usage

Pipe stdout through `grep` to extract the verdict:

```bash
homescreen -b $(pwd)/build/flutter_assets 2>/dev/null | grep '^WATCHDOG_TEST:'
```

The process exits cleanly in all cases — `PASS`, `FAIL`, and `SKIP` are all
normal termination.

## UI

The app shows a full-screen list of check rows:

- ⬤ grey — pending (not yet run)
- ⬤ green — pass
- ⬤ red — fail
- ⬤ amber — skip

A refresh button in the AppBar re-runs all checks.  The bottom bar shows
aggregate pass / fail / skip counts.
