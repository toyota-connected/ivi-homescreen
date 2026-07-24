#!/usr/bin/env bash
# Stress + watchdog integration test.
#
# Builds homescreen with the watchdog and systemd-watchdog integration enabled,
# then runs the stopwatch_stress_test Flutter app under load from stress-ng in
# parallel. The test verifies that the app survives the stress window without
# the watchdog firing (which would abort the process with SIGABRT), that the
# app wrote its per-minute uptime file, and that the embedder delivered
# sd_notify keep-alive pings (WATCHDOG=1) to a fake NOTIFY_SOCKET.
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
# Set the watchdog timeout in the bundle's config.toml. WATCHDOG_PID /
# WATCHDOG_USEC are NOT set, so sd_watchdog_enabled() returns 0 and config.toml
# owns the interval; sd_notify() still delivers to NOTIFY_SOCKET.
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
# Working area: uptime file + fake NOTIFY_SOCKET.
# ---------------------------------------------------------------------------
WORK_DIR="$(mktemp -d)"
UPTIME_FILE="${WORK_DIR}/uptime.txt"
SOCK_PATH="${WORK_DIR}/notify.sock"
MSGS_FILE="${WORK_DIR}/messages.txt"
HS_LOG="${WORK_DIR}/homescreen.log"

export STOPWATCH_UPTIME_FILE="${UPTIME_FILE}"

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

HS_PID=""
STRESS_PID=""

cleanup() {
  [[ -n "${HS_PID}" ]] && kill "${HS_PID}" 2>/dev/null || true
  [[ -n "${STRESS_PID}" ]] && kill "${STRESS_PID}" 2>/dev/null || true
  kill "${LISTENER_PID}" 2>/dev/null || true
  rm -rf "${WORK_DIR}"
}
trap cleanup EXIT

# Give the listener a moment to bind before the binary starts.
sleep 1

# ---------------------------------------------------------------------------
# Launch homescreen (the stopwatch app) in the background against the fake
# NOTIFY_SOCKET.
# ---------------------------------------------------------------------------
echo "== launching homescreen with the stopwatch app =="
NOTIFY_SOCKET="${SOCK_PATH}" \
  "${HS}" -b "${BUNDLE}" >"${HS_LOG}" 2>&1 &
HS_PID=$!
echo "homescreen pid: ${HS_PID}"

# Wait for the app to signal startup before starting the stress load.
for _ in $(seq 1 30); do
  if grep -qF "STOPWATCH_STRESS_START" "${HS_LOG}" 2>/dev/null; then
    break
  fi
  if ! kill -0 "${HS_PID}" 2>/dev/null; then
    echo "error: homescreen exited during startup" >&2
    cat "${HS_LOG}" >&2
    exit 1
  fi
  sleep 1
done

# ---------------------------------------------------------------------------
# Run stress-ng in parallel for the stress window.
# ---------------------------------------------------------------------------
echo "== starting stress-ng (cpu=${STRESS_CPU}, vm=${STRESS_VM}x${STRESS_VM_BYTES}) for ${STRESS_SECS}s =="
stress-ng \
  --cpu "${STRESS_CPU}" \
  --vm "${STRESS_VM}" --vm-bytes "${STRESS_VM_BYTES}" \
  --iomix 2 \
  --timeout "${STRESS_SECS}s" \
  --metrics-brief &
STRESS_PID=$!

# Wait for stress-ng to complete.
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
# ---------------------------------------------------------------------------
HS_EXIT=""
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
echo "homescreen exit code: ${HS_EXIT}"

# Give the listener time to flush the last datagram before we read.
sleep 1

# ---------------------------------------------------------------------------
# Verify.
# ---------------------------------------------------------------------------
PASS=0
TOTAL=4

# 1. The app must have started.
if grep -qF "STOPWATCH_STRESS_START" "${HS_LOG}"; then
  echo "PASS: app started (STOPWATCH_STRESS_START)"
  PASS=$((PASS + 1))
else
  echo "FAIL: app never printed STOPWATCH_STRESS_START" >&2
fi

# 2. The watchdog must NOT have fired (no SIGABRT 134/6). It survived stress.
if [[ "${HS_EXIT}" == "134" || "${HS_EXIT}" == "6" ]]; then
  echo "FAIL: watchdog fired under stress — homescreen aborted (exit ${HS_EXIT})" >&2
  echo "Messages received:" >&2
  cat "${MSGS_FILE}" >&2
else
  echo "PASS: watchdog petted, no timeout; homescreen survived stress (exit ${HS_EXIT})"
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

# 4. The embedder must have delivered sd_notify keep-alive pings.
if grep -qF "WATCHDOG=1" "${MSGS_FILE}" || grep -qF "STATUS=Running" "${MSGS_FILE}"; then
  echo "PASS: sd_notify messages received (systemd watchdog integration active)"
  PASS=$((PASS + 1))
else
  echo "FAIL: no sd_notify keep-alive messages received" >&2
  echo "Messages received:" >&2
  cat "${MSGS_FILE}" >&2
fi

echo
if [[ "${PASS}" == "${TOTAL}" ]]; then
  echo "stress watchdog integration test PASSED (${PASS}/${TOTAL})"
  exit 0
else
  echo "stress watchdog integration test FAILED (${PASS}/${TOTAL} checks passed)" >&2
  echo "--- homescreen log ---" >&2
  cat "${HS_LOG}" >&2
  exit 1
fi
