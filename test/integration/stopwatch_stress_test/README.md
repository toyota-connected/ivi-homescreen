# stopwatch_stress_test

Integration test app for exercising the ivi-homescreen **watchdog** under
system stress.

The embedder is built with `-DBUILD_WATCHDOG=ON` and
`-DBUILD_SYSTEMD_WATCHDOG=ON`. While this app runs, the driver script
(`scripts/stress_watchdog_integration.sh`) runs `stress-ng` in parallel to load
the CPU, memory and I/O. If the main or render thread is starved past the
watchdog timeout, the embedder aborts (SIGABRT). The test passes when the app
stays responsive for the whole stress window.

### Launch modes (chosen at runtime from `HAS_SYSTEMD`)

The driver script detects whether it is running inside a systemd session and
picks one of two modes:

- **`HAS_SYSTEMD=1` — real systemd service.** homescreen is launched as a
  transient `systemd-run --user` `Type=notify` service with `WatchdogSec`.
  systemd itself owns `NOTIFY_SOCKET` and the watchdog interval, so the true
  `sd_watchdog_enabled()` path runs: the embedder completes the `READY=1`
  handshake and must keep sending `WATCHDOG=1` pings or systemd kills the unit
  (`Result=watchdog`).
- **`HAS_SYSTEMD=0` — fake socket.** homescreen runs in the background against
  a fake `NOTIFY_SOCKET` listener that only records `sd_notify()` datagrams.
  systemd is not involved; this exercises message delivery only.

### Backend selection (chosen at runtime from `HAS_WAYLAND`)

The driver script also detects whether it is running inside a Wayland session
(`WAYLAND_DISPLAY` is set) and configures the embedder backend accordingly:

- **`HAS_WAYLAND=1` — Wayland session.** homescreen is built with the
  `wayland-egl` backend (`-DBUILD_BACKEND_WAYLAND_EGL=ON`,
  `-DBUILD_BACKEND_SOFTWARE=OFF`) and rendered through the compositor, so the
  stress window also loads the **GPU** render path. `WAYLAND_DISPLAY` is
  forwarded into the systemd unit when running in systemd mode.
- **`HAS_WAYLAND=0` — no Wayland session.** homescreen is built with the
  headless **software** backend (`-DBUILD_BACKEND_SOFTWARE=ON`,
  `-DBUILD_BACKEND_WAYLAND_EGL=OFF`); there is no GPU stress. This is the
  default for headless CI runners.

## What the app does

- Displays a **stopwatch** showing how long the app has been running.
- Shows an animated **spinner** (`CircularProgressIndicator`).
- Appends the current uptime to a file **once every minute**.
- Registers a watchdog source via the `watchdog` `MethodChannel` and pets it on
  a 1 s timer so the app survives the stress window.
- Provides a **"Stop petting watchdog"** button. Pressing it cancels the pet
  timer, leaving the source registered but starved. The embedder's watchdog
  thread then fires once the timeout elapses and aborts the process
  (**SIGABRT**, exit code 134), which the test harness sees as an error. The
  `STOPWATCH_STRESS_STOP_PET` marker is flushed the moment the button is
  pressed so the harness can attribute the abort to it.

The uptime file path is taken from the `STOPWATCH_UPTIME_FILE` environment
variable (set by the test script), falling back to
`/tmp/stopwatch_stress_uptime.txt`. Each line looks like:

```
2026-07-24T17:00:00.000000 uptime_seconds=60
```

## CI output

Each marker is printed to stdout and also appended (synchronously, `flush:true`)
to the file named by `STOPWATCH_STATUS_FILE` when set, so it survives stdout
buffering under systemd. The driver script streams the homescreen log live as it
is produced (prefixed with `[hs]`).

| Marker | Meaning |
|---|---|
| `STOPWATCH_STRESS_START` | Emitted once at startup |
| `STOPWATCH_STRESS_TICK <seconds>` | Emitted every minute after writing the file |
| `STOPWATCH_STRESS_WRITE_FAIL <e>` | Emitted if a file write throws |
| `STOPWATCH_STRESS_STOP_PET` | Emitted when the button cancels the pet timer |
| `STOPWATCH_STRESS_WATCHDOG_UNAVAILABLE <e>` | Emitted if the watchdog channel is absent (`BUILD_WATCHDOG=OFF`) |

## Building

### Prerequisites

```bash
# The embedder must be built with watchdog + systemd watchdog support
cmake -DBUILD_WATCHDOG=ON -DBUILD_SYSTEMD_WATCHDOG=ON ...
ninja -j$(nproc)
```

`stress-ng` must be installed and on `PATH`.

### Flutter app

```bash
cd test/integration/stopwatch_stress_test
flutter pub get
flutter build bundle
```

## Running

The full test is driven by the script:

```bash
scripts/stress_watchdog_integration.sh
```

Tunable via environment variables:

| Variable | Default | Meaning |
|---|---|---|
| `STRESS_SECS` | `180` | Length of the stress window (seconds) |
| `WATCHDOG_MS` | `5000` | Watchdog timeout written to the bundle `config.toml` |
| `STRESS_CPU` | `nproc` | Number of `stress-ng` CPU workers |
| `STRESS_VM` | `2` | Number of `stress-ng` VM workers |
| `STRESS_VM_BYTES` | `256M` | Memory per VM worker |
| `IHS_LOG_LEVEL` | `DEBUG` | Homescreen log level |
| `WATCHDOG_SEC` | `ceil(WATCHDOG_MS/1000)` | systemd `WatchdogSec` (systemd mode only) |

The script:

1. Builds the app bundle with `emb`.
2. Configures + builds homescreen with `BUILD_WATCHDOG=ON` and
   `BUILD_SYSTEMD_WATCHDOG=ON` (Wayland-EGL backend under a Wayland session,
   otherwise the software backend, headless).
3. Writes `[watchdog] timeout_ms` into the bundle `config.toml`.
4. Picks a launch mode from `HAS_SYSTEMD` (see above), launches homescreen,
   then runs `stress-ng` in parallel for the stress window.
5. Verifies: the app started, the watchdog did **not** fire (no SIGABRT and,
   in systemd mode, no `Result=watchdog` kill), the uptime file was written
   (when `STRESS_SECS >= 60`), and the systemd-watchdog integration was active
   (systemd `Type=notify` handshake sustained the unit, or `sd_notify`
   keep-alive datagrams were recorded in fake-socket mode).
