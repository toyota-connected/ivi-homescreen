# stopwatch_stress_test

Integration test app for exercising the ivi-homescreen **watchdog** under
system stress.

The embedder is built with `-DBUILD_WATCHDOG=ON` and
`-DBUILD_SYSTEMD_WATCHDOG=ON`. While this app runs, the driver script
(`scripts/stress_watchdog_integration.sh`) runs `stress-ng` in parallel to load
the CPU, memory and I/O. If the main or render thread is starved past the
watchdog timeout, the embedder aborts (SIGABRT). The test passes when the app
stays responsive for the whole stress window.

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
  a 1 s timer.
- Provides a **"Stop petting watchdog (crash)"** button. Pressing it cancels
  the pet timer, leaving the source registered but starved. The embedder's
  watchdog thread then fires after the timeout and aborts the process
  (SIGABRT) — the intended way to provoke a crash for testing.

The uptime file path is taken from the `STOPWATCH_UPTIME_FILE` environment
variable (set by the test script), falling back to
`/tmp/stopwatch_stress_uptime.txt`. Each line looks like:

```
2026-07-24T17:00:00.000000 uptime_seconds=60
```

## CI output

Printed to stdout:

| Marker | Meaning |
|---|---|
| `STOPWATCH_STRESS_START` | Printed once at startup |
| `STOPWATCH_STRESS_TICK <seconds>` | Printed every minute after writing the file |
| `STOPWATCH_STRESS_WRITE_FAIL <e>` | Printed if a file write throws |
| `STOPWATCH_STRESS_STOP_PET` | Printed when the button cancels the pet timer |
| `STOPWATCH_STRESS_WATCHDOG_UNAVAILABLE <e>` | Printed if the watchdog channel is absent (`BUILD_WATCHDOG=OFF`) |

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

The script:

1. Builds the app bundle with `emb`.
2. Configures + builds homescreen with `BUILD_WATCHDOG=ON` and
   `BUILD_SYSTEMD_WATCHDOG=ON` (Wayland-EGL backend under a Wayland session,
   otherwise the software backend, headless).
3. Writes `[watchdog] timeout_ms` into the bundle `config.toml`.
4. Starts a fake `NOTIFY_SOCKET` listener, launches homescreen, then runs
   `stress-ng` in parallel for the stress window.
5. Verifies: the app started, the watchdog did **not** fire (no SIGABRT), the
   uptime file was written (when `STRESS_SECS >= 60`), and `sd_notify`
   keep-alive messages were received.
