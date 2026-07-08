#!/usr/bin/env bash
# Crash-handler integration test — mirrors the crash-handler job in
# .github/workflows/oss-integration.yaml for local / Docker reproduction.
#
# Intended to be run inside a Docker container via:
#   /home/jamie/workspace-automation/run-docker.sh ubuntu:22.04 amd64 \
#     "scripts/crash_handler_integration.sh"
#
# Prerequisites (handled by flutter_workspace.py before this script runs):
#   - sentry-native built under ${FLUTTER_WORKSPACE}/app/sentry-native/
#   - ivi-homescreen sources at ${FLUTTER_WORKSPACE}/app/ivi-homescreen/
#   - material_3_demo at ${FLUTTER_WORKSPACE}/app/samples/material_3_demo/
#
# Environment variables (all optional):
#   KENT_HOST  — host for the fake Sentry server (default: 127.0.0.1)
#   KENT_PORT  — port for the fake Sentry server (default: 5000)

set -euo pipefail

KENT_HOST="${KENT_HOST:-127.0.0.1}"
KENT_PORT="${KENT_PORT:-5000}"

IVI_SRC="${FLUTTER_WORKSPACE}/app/ivi-homescreen"
IVI_BUILD="${IVI_SRC}/build"
SENTRY_LIBDIR="${FLUTTER_WORKSPACE}/app/sentry-native/build/release/staging"
CRASHPAD_BINDIR="${FLUTTER_WORKSPACE}/app/sentry-native/build/release/crashpad_build/handler"
SAMPLES_DIR="${FLUTTER_WORKSPACE}/app/flutter-samples/material_3_demo"
BUNDLE_DIR="${SAMPLES_DIR}/.desktop-homescreen"

KENT_PID=""

cleanup() {
  if [[ -n "${KENT_PID}" ]]; then
    kill "${KENT_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

_dir=$(pwd)

# ---------------------------------------------------------------------------
# Prepare test app
# ---------------------------------------------------------------------------
cd "${SAMPLES_DIR}"
# this is required to make sure a `.desktop-homescreen` bundle is generated
flutter pub get
flutter build bundle
flutter install -d desktop-homescreen
cd "${_dir}"

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------

cmake \
  -S "${IVI_SRC}" \
  -B "${IVI_BUILD}" \
  -D BUILD_BACKEND_WAYLAND_EGL=OFF \
  -D BUILD_BACKEND_WAYLAND_VULKAN=OFF \
  -D BUILD_BACKEND_DRM_KMS_EGL=OFF \
  -D BUILD_BACKEND_SOFTWARE=ON \
  -D BUILD_CRASH_HANDLER=ON \
  -D INTEGRATION_TEST_CRASH_HANDLER=ON

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
ninja -C "${IVI_BUILD}"

# ---------------------------------------------------------------------------
# Install and start Kent (fake Sentry server)
# ---------------------------------------------------------------------------
pip install kent

kent-server run --host "${KENT_HOST}" --port "${KENT_PORT}" &
KENT_PID=$!
sleep 2

run_and_verify_crash() {
  # ---------------------------------------------------------------------------
  # Run homescreen (expected to crash)
  # ---------------------------------------------------------------------------
  set +e
  SENTRY_DSN="http://public@${KENT_HOST}:${KENT_PORT}/1" \
    "${IVI_BUILD}/shell/homescreen" -b "${BUNDLE_DIR}"
  HOMESCREEN_EXIT_CODE=$?
  set -e

  if [[ "${HOMESCREEN_EXIT_CODE}" == "0" ]]; then
    echo "error: homescreen exited with 0 — expected a crash (non-zero exit)" >&2
    return 1
  fi

  _expected_exit_code=139
  if [[ "${HOMESCREEN_EXIT_CODE}" != "${_expected_exit_code}" ]]; then
    echo "error: homescreen exited with ${HOMESCREEN_EXIT_CODE} — expected ${_expected_exit_code}" >&2
    return 1
  else
    echo "homescreen exited with expected crash code ${_expected_exit_code}"
  fi

  # ---------------------------------------------------------------------------
  # Wait for crash report delivery, then verify Kent received it
  # ---------------------------------------------------------------------------
  sleep 10

  response=$(curl -sf "http://${KENT_HOST}:${KENT_PORT}/api/eventlist/")
  echo "Kent event list: ${response}"
  # response = { "events" : [ { "event_id" : "..." }, ... ] }
  # use jq to get length of events array
  event_count=$(echo "${response}" | jq '.events | length' || echo "0")
  if [[ "${event_count}" == "0" ]]; then
    echo "error: no crash reports received by Kent" >&2
    return 1
  fi
  echo "Kent received ${event_count} crash report(s) as expected"
}

for attempt in 1 2; do
  if run_and_verify_crash; then
    break
  fi
  if [[ "${attempt}" == "2" ]]; then
    echo "error: crash handler verification failed after ${attempt} attempts" >&2
    exit 1
  fi
  echo "Attempt ${attempt} failed, retrying..."
done