#!/usr/bin/env bash
# Watchdog integration test — compiles homescreen with watchdog enabled
#   and runs the watchdog integration test.
#
# Prerequisites (handled by flutter_workspace.py before this script runs):
#   - homescreen built with watchdog and software backend enabled at ${FLUTTER_WORKSPACE}/app/ivi-homescreen/build/shell/homescreen
#   - ivi-homescreen sources at ${FLUTTER_WORKSPACE}/app/ivi-homescreen/
#   - watchdog-test at ${FLUTTER_WORKSPACE}/app/ivi-homescreen/test/integration/watchdog_test/
#

set -euo pipefail

IVI_SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IVI_BUILD="${IVI_SRC}/build"
TEST_NAME="watchdog_test"
TEST_DIR="${IVI_SRC}/test/integration/${TEST_NAME}"
BUNDLE_DIR="${FLUTTER_WORKSPACE}/bundle/${TEST_NAME}-debug-$(uname -i)"


# ---------------------------------------------------------------------------
# Prepare test app
# ---------------------------------------------------------------------------
emb bundle --app-path "$TEST_DIR" -m debug --build

# ---------------------------------------------------------------------------
# Configure
# ---------------------------------------------------------------------------
cmake \
  -S "${IVI_SRC}" \
  -B "${IVI_BUILD}" \
  -G Ninja \
  -D BUILD_BACKEND_WAYLAND_EGL=OFF \
  -D BUILD_BACKEND_WAYLAND_VULKAN=OFF \
  -D BUILD_BACKEND_DRM_KMS_EGL=OFF \
  -D BUILD_BACKEND_SOFTWARE=ON \
  -D BUILD_WATCHDOG=ON

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
ninja -C "${IVI_BUILD}"

# ---------------------------------------------------------------------------
# Run homescreen (could crash)
# ---------------------------------------------------------------------------
set +e
"${IVI_BUILD}/shell/homescreen" -b "${BUNDLE_DIR}"
HOMESCREEN_EXIT_CODE=$?
set -e

if [[ "${HOMESCREEN_EXIT_CODE}" == "0" ]]; then
  echo "success: homescreen exited with 0 — all tests pass"
  exit 0
else
  echo "homescreen exited with expected crash code ${HOMESCREEN_EXIT_CODE}"
  exit 1
fi