#!/usr/bin/env bash
# Systemd watchdog integration test.
#
# Builds this source tree with the systemd watchdog integration test mode
# enabled, then runs the binary against a fake sd_notify socket and verifies
# that the watchdog timeout path fires correctly.
#
# What the test does NOT need, and why:
#   * No Flutter app. The integration path fires in main() right after watchdog
#     init — before App construction — so there is no Dart content to compile.
#   * No libflutter_engine.so. The engine is dlopened lazily at engine
#     start-up, which is past the test exit point.
#   * No Flutter SDK.
#
# How it works:
#   1. A minimal bundle config.toml sets [watchdog] timeout_ms = 1000 so the
#      watchdog fires quickly without using a real systemd unit.
#   2. WATCHDOG_PID / WATCHDOG_USEC are deliberately NOT set, so
#      sd_watchdog_enabled() returns 0 and config.toml owns the interval.
#      sd_notify() still delivers messages to NOTIFY_SOCKET unconditionally.
#   3. A Python process listens on a Unix datagram socket (NOTIFY_SOCKET) and
#      records every message the binary sends.
#   4. The binary registers source 100 and never pets it. After ~3× the
#      timeout, the watchdog thread sends WATCHDOG=trigger and calls abort().
#      The script verifies exit code 134 (SIGABRT) and the trigger message.
#
# Env (all optional):
#   IVI_SRC     ivi-homescreen source root (default: directory above this script)
#   IVI_BUILD   build directory (default: $IVI_SRC/build-systemd-watchdog-test)

set -euo pipefail

IVI_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IVI_BUILD="${IVI_SRC}/build"
HS="${IVI_BUILD}/shell/homescreen"

# ── Build ──────────────────────────────────────────────────────────────────
cmake \
  -S "${IVI_SRC}" \
  -B "${IVI_BUILD}" \
  -G Ninja \
  -DBUILD_BACKEND_WAYLAND_EGL=OFF \
  -DBUILD_BACKEND_WAYLAND_VULKAN=OFF \
  -DBUILD_BACKEND_DRM_KMS_EGL=OFF \
  -DBUILD_BACKEND_SOFTWARE=ON \
  -DBUILD_WATCHDOG=ON \
  -DBUILD_SYSTEMD_WATCHDOG=ON \
  -DINTEGRATION_TEST_SYSTEMD_WATCHDOG=ON

ninja -C "${IVI_BUILD}"

[[ -x "${HS}" ]] || {
  echo "error: homescreen binary not found at ${HS}" >&2
  exit 2
}
echo "homescreen binary: ${HS}"

# ── Bundle ─────────────────────────────────────────────────────────────────
# timeout_ms = 1000 ms keeps total test time under 5 s (sleep = 3× = 3 s).
# No other keys are required — the binary exits before any Flutter content is
# loaded.
BUNDLE="$(mktemp -d)"
cat > "${BUNDLE}/config.toml" <<'TOML'
[global]
app_id = 'systemd-watchdog-test'

[[view]]
width = 1
height = 1

[watchdog]
timeout_ms = 1000
TOML

# ── Fake NOTIFY_SOCKET ─────────────────────────────────────────────────────
SOCK_DIR="$(mktemp -d)"
SOCK_PATH="${SOCK_DIR}/notify.sock"
MSGS_FILE="${SOCK_DIR}/messages.txt"

# Python listener: binds a Unix datagram socket, appends each received
# datagram (decoded as UTF-8) to MSGS_FILE, one message per line.
python3 - "${SOCK_PATH}" "${MSGS_FILE}" <<'PY' &
import socket, sys, pathlib

sock_path = sys.argv[1]
msgs_file = pathlib.Path(sys.argv[2])
msgs_file.write_text("")

sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
sock.bind(sock_path)
sock.settimeout(30)

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

cleanup() {
  kill "${LISTENER_PID}" 2>/dev/null || true
  rm -rf "${BUNDLE}" "${SOCK_DIR}"
}
trap cleanup EXIT

# Give the listener a moment to bind before the binary starts.
sleep 1

# ── Run ────────────────────────────────────────────────────────────────────
# Timeout is generous: 1 s interval × 3 = 3 s abort window + headroom.
set +e
NOTIFY_SOCKET="${SOCK_PATH}" \
  timeout -k 5 20 "${HS}" -b "${BUNDLE}"
EXIT_CODE=$?
set -e

echo "homescreen exit code: ${EXIT_CODE}"

# Give the listener time to flush the last datagram before we read.
sleep 1

# ── Verify ─────────────────────────────────────────────────────────────────
PASS=0

# 1. Binary must have aborted (SIGABRT = 134, or 6 on some shells).
if [[ "${EXIT_CODE}" == "134" || "${EXIT_CODE}" == "6" ]]; then
  echo "PASS: binary aborted as expected (exit ${EXIT_CODE})"
  PASS=$((PASS + 1))
else
  echo "FAIL: expected abort (134/6), got ${EXIT_CODE}" >&2
fi

# 2. NOTIFY_SOCKET must have received STATUS=Running (from constructor).
if grep -qF "STATUS=Running" "${MSGS_FILE}"; then
  echo "PASS: STATUS=Running received"
  PASS=$((PASS + 1))
else
  echo "FAIL: STATUS=Running not received" >&2
fi

# 3. NOTIFY_SOCKET must have received WATCHDOG=trigger (from timeout handler).
if grep -qF "WATCHDOG=trigger" "${MSGS_FILE}"; then
  echo "PASS: WATCHDOG=trigger received"
  PASS=$((PASS + 1))
else
  echo "FAIL: WATCHDOG=trigger not received" >&2
  echo "Messages received:"
  cat "${MSGS_FILE}" >&2
fi

if [[ "${PASS}" == "3" ]]; then
  echo "systemd watchdog integration test PASSED"
  exit 0
else
  echo "systemd watchdog integration test FAILED (${PASS}/3 checks passed)" >&2
  exit 1
fi
