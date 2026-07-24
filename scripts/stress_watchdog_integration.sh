#!/usr/bin/env bash
# Stress + watchdog integration test.
#
# Builds homescreen with the watchdog and systemd-watchdog integration enabled,
# then runs the stopwatch_stress_test Flutter app under load from stress-ng in
# parallel. The test verifies that the app survives the stress window without
# the watchdog firing, that the app wrote its per-minute uptime file, and that
# the systemd-watchdog integration was active.
#
# Launch mode is chosen at runtime from HAS_SYSTEMD:
#   HAS_SYSTEMD=1  -> run homescreen as a real transient `systemd-run --user`
#                     Type=notify service with WatchdogSec set. systemd owns
#                     NOTIFY_SOCKET and the watchdog interval, exercising the
#                     true sd_watchdog_enabled() path (READY=1 handshake, and
#                     systemd killing the unit if keep-alives stop).
#   HAS_SYSTEMD=0  -> run homescreen in the background against a fake
#                     NOTIFY_SOCKET listener that records sd_notify() datagrams
#                     (message-delivery only; systemd is not involved).
#
# Prerequisites (handled by flutter_workspace.py / CI before this script runs):
#   - ivi-homescreen sources at ${FLUTTER_WORKSPACE}/app/ivi-homescreen/
#   - stopwatch_stress_test at
#       ${FLUTTER_WORKSPACE}/app/ivi-homescreen/test/integration/stopwatch_stress_test/
#   - emb on PATH with a provisioned Flutter; FLUTTER_WORKSPACE set.
#   - stress-ng installed and on PATH.
#
# Env (all optional):
#   IVI_SRC        ivi-homescreen source root (default: directory above this script)
#   IVI_BUILD      build directory (default: $IVI_SRC/build)
#   STRESS_SECS    how long to run the stress window in seconds (default: 180)
#   WATCHDOG_MS    watchdog timeout in ms written to the bundle (default: 5000)
#   STRESS_CPU     number of stress-ng CPU workers (default: nproc)
#   STRESS_VM      number of stress-ng VM workers (default: 2)
#   STRESS_VM_BYTES memory per VM worker (default: 256M)
#   IHS_LOG_LEVEL  homescreen log level (default: DEBUG)
#   WATCHDOG_SEC   systemd WatchdogSec in seconds (systemd mode only; default:
#                  ceil(WATCHDOG_MS/1000))

set -euo pipefail

IVI_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IVI_BUILD="${IVI_BUILD:-${IVI_SRC}/build}"
TEST_NAME="stopwatch_stress_test"
TEST_DIR="${IVI_SRC}/test/integration/${TEST_NAME}"
HS="${IVI_BUILD}/shell/homescreen"

STRESS_SECS="${STRESS_SECS:-180}"
WATCHDOG_MS="${WATCHDOG_MS:-5000}"
STRESS_CPU="${STRESS_CPU:-$(nproc)}"
STRESS_VM="${STRESS_VM:-2}"
STRESS_VM_BYTES="${STRESS_VM_BYTES:-256M}"
IHS_LOG_LEVEL="${IHS_LOG_LEVEL:-DEBUG}"

BUNDLE_ROOT="${FLUTTER_WORKSPACE:-${IVI_SRC}}/bundle"

command -v stress-ng >/dev/null 2>&1 || {
  echo "error: stress-ng not found on PATH" >&2
  exit 2
}

# ---------------------------------------------------------------------------
# Build the test app bundle. emb names the output stopwatch_stress_test-debug-
# <arch>; resolve it by glob rather than guessing the arch suffix.
# ---------------------------------------------------------------------------
echo "== building ${TEST_NAME} bundle =="
emb bundle --app-path "${TEST_DIR}" -m debug --build

shopt -s nullglob
bundles=( "${BUNDLE_ROOT}"/${TEST_NAME}-debug-* )
shopt -u nullglob
[[ ${#bundles[@]} -ge 1 ]] || {
  echo "error: no ${TEST_NAME} bundle under ${BUNDLE_ROOT}" >&2
  exit 2
}
BUNDLE="${bundles[0]}"
echo "bundle: ${BUNDLE}"

# Check if running in a Wayland session (write to HAS_WAYLAND)
if [ -n "${WAYLAND_DISPLAY:-}" ]; then
  HAS_WAYLAND=1
  echo "=== VARIATION: Wayland session detected, will run WAYLAND_EGL backend and stress GPU ==="
else
  HAS_WAYLAND=0
  echo "=== VARIATION: Wayland session not detected, will run SOFTWARE backend (no GPU stress) ==="
fi

# Check if running in a systemd session (write to HAS_SYSTEMD)
if [ -n "${XDG_SESSION_ID:-}" ] && [ -S "/run/systemd/private" ]; then
  HAS_SYSTEMD=1
  echo "=== VARIATION: systemd session detected, will run real SYSTEMD watchdog ==="
else
  HAS_SYSTEMD=0
  echo "=== VARIATION: systemd session not detected, will fake SYSTEMD watchdog ==="
fi

# ---------------------------------------------------------------------------
# Launch mode. When HAS_SYSTEMD=1 we run homescreen as a real transient
# `systemd-run --user` Type=notify service with WatchdogSec set, so systemd
# itself owns NOTIFY_SOCKET and the watchdog interval — this exercises the true
# sd_watchdog_enabled() path (systemdIntervalActive_ = true, READY=1 handshake).
# When HAS_SYSTEMD=0 we fall back to a fake NOTIFY_SOCKET listener that only
# exercises sd_notify() message delivery.
# ---------------------------------------------------------------------------
if [[ "${HAS_SYSTEMD}" == "1" ]] && command -v systemd-run >/dev/null 2>&1; then
  MODE="systemd"
else
  MODE="fake-socket"
  if [[ "${HAS_SYSTEMD}" == "1" ]]; then
    echo "warning: HAS_SYSTEMD=1 but systemd-run not found; using fake-socket mode" >&2
    HAS_SYSTEMD=0
  fi
fi
echo "watchdog test launch mode: ${MODE}"

# Transient unit name, used only in systemd mode.
UNIT="ivi-stopwatch-stress-$$.service"

# ---------------------------------------------------------------------------
# Configure + build homescreen with watchdog + systemd watchdog enabled.
# Software backend keeps the test headless. INTEGRATION_TEST_SYSTEMD_WATCHDOG
# is deliberately OFF: that mode aborts in main() before any app runs, whereas
# here we want the full app + engine running under stress.
# ---------------------------------------------------------------------------
echo "== configuring homescreen (BUILD_WATCHDOG=ON, BUILD_SYSTEMD_WATCHDOG=ON) =="
cmake \
  -S "${IVI_SRC}" \
  -B "${IVI_BUILD}" \
  -G Ninja \
  -D BUILD_BACKEND_WAYLAND_EGL=$( [[ "${HAS_WAYLAND}" == "1" ]] && echo ON || echo OFF) \
  -D BUILD_BACKEND_WAYLAND_VULKAN=OFF \
  -D BUILD_BACKEND_DRM_KMS_EGL=OFF \
  -D BUILD_BACKEND_SOFTWARE=$( [[ "${HAS_WAYLAND}" == "1" ]] && echo OFF || echo ON) \
  -D BUILD_WATCHDOG=ON \
  -D BUILD_SYSTEMD_WATCHDOG=ON

echo "== building homescreen =="
ninja -C "${IVI_BUILD}"

[[ -x "${HS}" ]] || {
  echo "error: homescreen binary not found at ${HS}" >&2
  exit 2
}
echo "homescreen binary: ${HS}"

export LD_LIBRARY_PATH="${IVI_BUILD}/shared:${IVI_BUILD}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

# ---------------------------------------------------------------------------
# Set the watchdog timeout in the bundle's config.toml.
#   fake-socket mode: WATCHDOG_USEC is NOT set, so sd_watchdog_enabled() returns
#                     0 and config.toml owns the interval; sd_notify() still
#                     delivers to the fake NOTIFY_SOCKET.
#   systemd mode:     systemd sets WATCHDOG_USEC from WatchdogSec, so
#                     sd_watchdog_enabled() > 0 and the systemd interval wins;
#                     this timeout_ms is then ignored by the embedder (harmless).
# ---------------------------------------------------------------------------
if grep -q '^\[watchdog\]' "${BUNDLE}/config.toml" 2>/dev/null; then
  echo "config.toml already has a [watchdog] section; leaving it as-is"
else
  cat >> "${BUNDLE}/config.toml" <<TOML

[watchdog]
timeout_ms = ${WATCHDOG_MS}
TOML
fi

# ---------------------------------------------------------------------------
# Working area: uptime file, log, and (fake-socket mode only) NOTIFY_SOCKET.
# ---------------------------------------------------------------------------
WORK_DIR="$(mktemp -d)"
UPTIME_FILE="${WORK_DIR}/uptime.txt"
SOCK_PATH="${WORK_DIR}/notify.sock"
MSGS_FILE="${WORK_DIR}/messages.txt"
HS_LOG="${WORK_DIR}/homescreen.log"
# The app mirrors its CI markers here synchronously (flush:true), so they are
# reliable even when stdout is block-buffered under systemd. This is the
# authoritative source for the STOPWATCH_STRESS_* markers.
STATUS_FILE="${WORK_DIR}/status.txt"
: >"${MSGS_FILE}"
: >"${STATUS_FILE}"

export STOPWATCH_UPTIME_FILE="${UPTIME_FILE}"
export STOPWATCH_STATUS_FILE="${STATUS_FILE}"

HS_PID=""
STRESS_PID=""
LISTENER_PID=""
TAIL_PID=""

# Create the log up front so the live streamer can follow it from line one.
: >"${HS_LOG}"

# In systemd mode the unit writes stdout/stderr straight to HS_LOG via
# StandardOutput=append (see the launch below), and also merge the journal in
# as a fallback in case the append target is unavailable. In fake-socket mode
# the process already redirects to HS_LOG, so this is a no-op there.
hs_snapshot_log() {
  if [[ "${MODE}" == "systemd" ]]; then
    if [[ ! -s "${HS_LOG}" ]]; then
      journalctl --user -u "${UNIT}" --no-pager -o cat >"${HS_LOG}" 2>/dev/null || true
    fi
  fi
}

# The authoritative marker source: the app's flushed status file (reliable
# under systemd where stdout is buffered). Falls back to HS_LOG.
has_marker() {
  grep -qF "$1" "${STATUS_FILE}" 2>/dev/null || grep -qF "$1" "${HS_LOG}" 2>/dev/null
}

# True while homescreen is active OR still starting up.
hs_is_running() {
  if [[ "${MODE}" == "systemd" ]]; then
    local st
    st="$(systemctl --user show "${UNIT}" -p ActiveState --value 2>/dev/null)"
    [[ "${st}" == "active" || "${st}" == "activating" ]]
  else
    [[ -n "${HS_PID}" ]] && kill -0 "${HS_PID}" 2>/dev/null
  fi
}

cleanup() {
  [[ -n "${STRESS_PID}" ]] && kill "${STRESS_PID}" 2>/dev/null || true
  if [[ "${MODE}" == "systemd" ]]; then
    systemctl --user stop "${UNIT}" 2>/dev/null || true
    systemctl --user reset-failed "${UNIT}" 2>/dev/null || true
  else
    [[ -n "${HS_PID}" ]] && kill "${HS_PID}" 2>/dev/null || true
  fi
  [[ -n "${LISTENER_PID}" ]] && kill "${LISTENER_PID}" 2>/dev/null || true
  # Let the live streamer flush the final lines, then stop it.
  if [[ -n "${TAIL_PID}" ]]; then
    sleep 1
    kill "${TAIL_PID}" 2>/dev/null || true
  fi
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

if [[ "${MODE}" == "fake-socket" ]]; then
  # Python listener: binds the NOTIFY_SOCKET Unix datagram socket and records
  # every message the binary sends, one per line.
  python3 - "${SOCK_PATH}" "${MSGS_FILE}" <<'PY' &
import socket, sys, pathlib

sock_path = sys.argv[1]
msgs_file = pathlib.Path(sys.argv[2])
msgs_file.write_text("")

sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
sock.bind(sock_path)
sock.settimeout(600)

try:
    while True:
        data, _ = sock.recvfrom(4096)
        msg = data.decode("utf-8", errors="replace").strip()
        with msgs_file.open("a") as f:
            f.write(msg + "\n")
except socket.timeout:
    pass
finally:
    sock.close()
PY
  LISTENER_PID=$!
  # Give the listener a moment to bind before the binary starts.
  sleep 1
fi

# ---------------------------------------------------------------------------
# Launch homescreen (the stopwatch app).
#   systemd mode:     as a transient --user Type=notify service with
#                     WatchdogSec — systemd owns NOTIFY_SOCKET + interval.
#   fake-socket mode: in the background against the fake NOTIFY_SOCKET.
# ---------------------------------------------------------------------------
echo "== launching homescreen with the stopwatch app (${MODE}) =="
if [[ "${MODE}" == "systemd" ]]; then
  WATCHDOG_SEC="${WATCHDOG_SEC:-$(( (WATCHDOG_MS + 999) / 1000 ))}"
  # Build the systemd-run argument list in an array so conditional env vars
  # can be added safely without word-splitting hazards.
  run_args=(
    --user
    "--unit=${UNIT}"
    --service-type=notify
    -p "WatchdogSec=${WATCHDOG_SEC}"
    -p "Restart=no"
    -p "StandardOutput=append:${HS_LOG}"
    -p "StandardError=append:${HS_LOG}"
    -p "Environment=STOPWATCH_UPTIME_FILE=${UPTIME_FILE}"
    -p "Environment=STOPWATCH_STATUS_FILE=${STATUS_FILE}"
    -p "Environment=LD_LIBRARY_PATH=${LD_LIBRARY_PATH}"
    -p "Environment=IHS_LOG_LEVEL=${IHS_LOG_LEVEL}"
  )
  [[ -n "${WAYLAND_DISPLAY:-}" ]] && run_args+=( -p "Environment=WAYLAND_DISPLAY=${WAYLAND_DISPLAY}" )
  [[ -n "${XDG_RUNTIME_DIR:-}" ]] && run_args+=( -p "Environment=XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR}" )
  # systemd-run returns immediately after the job is queued; the unit then
  # starts asynchronously and we poll for readiness below.
  systemd-run "${run_args[@]}" "${HS}" -b "${BUNDLE}"
  echo "systemd unit: ${UNIT} (WatchdogSec=${WATCHDOG_SEC})"
else
  NOTIFY_SOCKET="${SOCK_PATH}" \
    "${HS}" -b "${BUNDLE}" >"${HS_LOG}" 2>&1 &
  HS_PID=$!
  echo "homescreen pid: ${HS_PID}"
fi

# Stream homescreen's log live as it is produced. Both modes write HS_LOG in
# real time (fake-socket via redirection, systemd via StandardOutput=append),
# so `tail -F` mirrors it to our stdout as lines arrive rather than dumping
# everything after the app exits.
echo "== streaming homescreen log (live) =="
tail -n +1 -F "${HS_LOG}" 2>/dev/null | sed 's/^/[hs] /' &
TAIL_PID=$!

# Wait for the app to signal startup before starting the stress load.
STARTED=0
for _ in $(seq 1 30); do
  hs_snapshot_log
  if has_marker "STOPWATCH_STRESS_START"; then
    STARTED=1
    break
  fi
  if ! hs_is_running; then
    echo "error: homescreen exited during startup (see [hs] log above)" >&2
    hs_snapshot_log
    sleep 1  # let the live streamer flush the final lines
    exit 1
  fi
  sleep 1
done

if [[ "${STARTED}" != "1" ]]; then
  echo "error: homescreen did not signal startup within 30s (see [hs] log above)" >&2
  hs_snapshot_log
  sleep 1  # let the live streamer flush the final lines
  exit 1
fi

# ---------------------------------------------------------------------------
# Run stress-ng in parallel for the stress window, but poll homescreen so we
# stop early (rather than hanging for the full window) if it dies — e.g. the
# watchdog fires or the crash button is pressed.
# ---------------------------------------------------------------------------
echo "== starting stress-ng (cpu=${STRESS_CPU}, vm=${STRESS_VM}x${STRESS_VM_BYTES}) for ${STRESS_SECS}s =="
stress-ng \
  --cpu "${STRESS_CPU}" \
  --vm "${STRESS_VM}" --vm-bytes "${STRESS_VM_BYTES}" \
  --iomix 2 \
  --timeout "${STRESS_SECS}s" \
  --metrics-brief &
STRESS_PID=$!

HS_DIED_DURING_STRESS=0
while kill -0 "${STRESS_PID}" 2>/dev/null; do
  if ! hs_is_running; then
    echo "homescreen exited during the stress window — stopping stress-ng early"
    HS_DIED_DURING_STRESS=1
    kill "${STRESS_PID}" 2>/dev/null || true
    break
  fi
  sleep 1
done

# Reap stress-ng.
set +e
wait "${STRESS_PID}"
STRESS_EXIT=$?
set -e
STRESS_PID=""
echo "stress-ng exit code: ${STRESS_EXIT}"

# Give the app one more moment to flush a final tick / notify.
sleep 2

# ---------------------------------------------------------------------------
# Stop homescreen and capture its exit status.
#   systemd mode:     read Result/ExecMainStatus/ExecMainCode from the unit;
#                     a watchdog kill shows Result=watchdog; a fatal signal
#                     (e.g. SIGABRT=6 from the embedder's abort(), or SIGSEGV=11
#                     from the deliberate "Crash now" button) shows
#                     ExecMainCode=dumped/killed with ExecMainStatus=<signal>.
#   fake-socket mode: wait on the background PID; a signal death yields exit
#                     code 128+signal (134=SIGABRT, 139=SIGSEGV).
#
# Outcome flags set here (consumed by the checks below):
#   WATCHDOG_KILLED  systemd killed the unit for a missed keep-alive
#   HS_CRASHED       the process died from any fatal signal (abnormal exit)
#   HS_SIGNAL        the terminating signal number (when known)
# ---------------------------------------------------------------------------
HS_EXIT=""
WATCHDOG_KILLED=0
HS_CRASHED=0
HS_SIGNAL=""
if [[ "${MODE}" == "systemd" ]]; then
  if hs_is_running; then
    echo "homescreen unit still running after stress window (good) — stopping it"
    systemctl --user stop "${UNIT}" 2>/dev/null || true
  fi
  RESULT="$(systemctl --user show "${UNIT}" -p Result --value 2>/dev/null)"
  MAIN_STATUS="$(systemctl --user show "${UNIT}" -p ExecMainStatus --value 2>/dev/null)"
  MAIN_CODE="$(systemctl --user show "${UNIT}" -p ExecMainCode --value 2>/dev/null)"
  echo "systemd unit result: Result=${RESULT} ExecMainCode=${MAIN_CODE} ExecMainStatus=${MAIN_STATUS}"
  hs_snapshot_log
  if [[ "${RESULT}" == "watchdog" ]]; then
    # systemd killed it because keep-alive pings stopped.
    WATCHDOG_KILLED=1
    HS_CRASHED=1
    HS_EXIT=134
  elif [[ "${MAIN_CODE}" == "dumped" || "${MAIN_CODE}" == "killed" ]]; then
    # Process died from a signal; ExecMainStatus holds the signal number.
    HS_CRASHED=1
    HS_SIGNAL="${MAIN_STATUS}"
    HS_EXIT="$(( 128 + MAIN_STATUS ))"
  else
    # Clean exit; ExecMainStatus is the process exit code.
    HS_EXIT="${MAIN_STATUS:-0}"
  fi
else
  if kill -0 "${HS_PID}" 2>/dev/null; then
    echo "homescreen still running after stress window (good) — stopping it"
    kill -INT "${HS_PID}" 2>/dev/null || true
    set +e
    wait "${HS_PID}"
    HS_EXIT=$?
    set -e
  else
    set +e
    wait "${HS_PID}"
    HS_EXIT=$?
    set -e
    echo "homescreen already exited with code ${HS_EXIT}"
  fi
  HS_PID=""
  # A fatal signal yields exit code 128+signal.
  if [[ "${HS_EXIT}" -gt 128 ]]; then
    HS_CRASHED=1
    HS_SIGNAL="$(( HS_EXIT - 128 ))"
  fi
fi
echo "homescreen exit code: ${HS_EXIT}"

# Give the listener time to flush the last datagram before we read.
sleep 1
hs_snapshot_log

# ---------------------------------------------------------------------------
# Verify.
# ---------------------------------------------------------------------------
PASS=0
TOTAL=4

# 1. The app must have started.
if has_marker "STOPWATCH_STRESS_START"; then
  echo "PASS: app started (STOPWATCH_STRESS_START)"
  PASS=$((PASS + 1))
else
  echo "FAIL: app never emitted STOPWATCH_STRESS_START" >&2
fi

# 2. homescreen must NOT have crashed. It survived the stress window.
#    Any abnormal (signal) exit is a failure:
#      - systemd watchdog kill (Result=watchdog),
#      - SIGABRT (6) from the embedder's own watchdog abort() — either because
#        stress starved a thread, or because the "Stop petting watchdog" button
#        was pressed (STOPWATCH_STRESS_STOP_PET emitted first),
#      - or any other fatal signal.
if [[ "${HS_CRASHED}" == "1" ]]; then
  if [[ "${WATCHDOG_KILLED}" == "1" ]]; then
    echo "FAIL: systemd watchdog fired — unit killed (Result=watchdog)" >&2
  elif has_marker "STOPWATCH_STRESS_STOP_PET"; then
    echo "FAIL: watchdog aborted the app after petting stopped (button) — signal ${HS_SIGNAL:-?}, exit ${HS_EXIT}" >&2
  elif [[ "${HS_SIGNAL}" == "6" ]]; then
    echo "FAIL: watchdog fired under stress — homescreen aborted (SIGABRT, exit ${HS_EXIT})" >&2
  else
    echo "FAIL: homescreen crashed under stress — signal ${HS_SIGNAL:-?}, exit ${HS_EXIT}" >&2
  fi
  [[ -s "${MSGS_FILE}" ]] && { echo "Messages received:" >&2; cat "${MSGS_FILE}" >&2; }
else
  echo "PASS: no crash; homescreen survived stress (exit ${HS_EXIT})"
  PASS=$((PASS + 1))
fi

# 3. The app must have written at least one uptime line to the file.
#    Only reliably checkable if STRESS_SECS is at least one full minute.
if [[ "${STRESS_SECS}" -ge 60 ]]; then
  if [[ -s "${UPTIME_FILE}" ]] && grep -qE 'uptime_seconds=[0-9]+' "${UPTIME_FILE}"; then
    LINES=$(grep -cE 'uptime_seconds=[0-9]+' "${UPTIME_FILE}")
    echo "PASS: uptime file written (${LINES} line(s))"
    echo "  --- ${UPTIME_FILE} ---"
    cat "${UPTIME_FILE}"
    PASS=$((PASS + 1))
  else
    echo "FAIL: uptime file empty or missing entries at ${UPTIME_FILE}" >&2
  fi
else
  echo "SKIP-COUNT: STRESS_SECS < 60, not expecting a per-minute write; counting as pass"
  PASS=$((PASS + 1))
fi

# 4. The systemd watchdog integration must have been active.
#    systemd mode:     the unit reached "active", which for Type=notify means
#                      the embedder sent READY=1 (sd_watchdog_enabled() path),
#                      and it kept sending WATCHDOG=1 pings for the whole run
#                      (otherwise systemd would have killed it — caught above).
#    fake-socket mode: verify the recorded datagrams contain a keep-alive.
if [[ "${MODE}" == "systemd" ]]; then
  if [[ "${WATCHDOG_KILLED}" == "0" ]] && has_marker "STOPWATCH_STRESS_START"; then
    echo "PASS: systemd Type=notify handshake succeeded and keep-alives sustained the unit"
    PASS=$((PASS + 1))
  else
    echo "FAIL: systemd watchdog integration did not sustain the unit (see [hs] log above)" >&2
    hs_snapshot_log
  fi
else
  if grep -qF "WATCHDOG=1" "${MSGS_FILE}" || grep -qF "STATUS=Running" "${MSGS_FILE}"; then
    echo "PASS: sd_notify messages received (systemd watchdog integration active)"
    PASS=$((PASS + 1))
  else
    echo "FAIL: no sd_notify keep-alive messages received" >&2
    echo "Messages received:" >&2
    cat "${MSGS_FILE}" >&2
  fi
fi

echo
if [[ "${PASS}" == "${TOTAL}" ]]; then
  echo "stress watchdog integration test PASSED (${PASS}/${TOTAL})"
  exit 0
else
  echo "stress watchdog integration test FAILED (${PASS}/${TOTAL} checks passed)" >&2
  echo "(homescreen log was streamed live above, prefixed with [hs])" >&2
  exit 1
fi
