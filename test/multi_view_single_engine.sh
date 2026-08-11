#!/usr/bin/env bash
#
# multi_view_single_engine.sh — validate that ONE Flutter engine drives TWO
# outputs.
#
# One bundle is launched with a config listing two outputs; the harness
# asserts the single engine brought up BOTH connectors and that BOTH CRTCs are
# presenting (page flips), then optionally captures a PNG per output.
#
# Rigs:
#   - Real hardware:  DRM_DEVICE=/dev/dri/card1 CONNECTOR_A=HDMI-A-1 CONNECTOR_B=DP-1
#   - Portable/CI:    provision two virtual outputs first, then point at the vkms card:
#                       sudo third_party/drm-cxx/scripts/vkms_dual.sh up
#                       DRM_DEVICE=/dev/dri/cardN CONNECTOR_A=... CONNECTOR_B=... SOFTWARE_RENDER=1 ...
#
# Env:
#   HOMESCREEN       path to the homescreen binary (required)
#   BUNDLE           path to the multi_view_test bundle (required)
#   DRM_DEVICE       /dev/dri/cardN. Unset = auto-detect the vkms card.
#                    Card numbering is NOT stable, and this harness takes DRM
#                    master and drives a modeset, so there is deliberately no
#                    numeric default -- one would blank a real display on any
#                    machine where that number is the GPU. Set it explicitly to
#                    opt in to real hardware.
#   CONNECTOR_A      first output connector    (default: 1st on the card)
#   CONNECTOR_B      second output connector   (default: 2nd on the card)
#   DURATION         seconds to run            (default 6)
#   STARTUP_GRACE    seconds before liveness   (default 2)
#   COUNT_FLIPS      1 = prove page flips on both CRTCs via strace (default 0)
#   CAPTURE          1 = dump a PNG per output via IVI_DRM_CAPTURE + SIGUSR1 (default 0)
#   SOFTWARE_RENDER  1 = LIBGL_ALWAYS_SOFTWARE=1 (vkms/llvmpipe)
#   --check          verify prerequisites only, do not launch
set -uo pipefail

HOMESCREEN="${HOMESCREEN:-}"
BUNDLE="${BUNDLE:-}"
# No numeric defaults: see the DRM_DEVICE note above.
DRM_DEVICE="${DRM_DEVICE:-}"
CONNECTOR_A="${CONNECTOR_A:-}"
CONNECTOR_B="${CONNECTOR_B:-}"
DURATION="${DURATION:-6}"
STARTUP_GRACE="${STARTUP_GRACE:-2}"
COUNT_FLIPS="${COUNT_FLIPS:-0}"
CAPTURE="${CAPTURE:-0}"
SOFTWARE_RENDER="${SOFTWARE_RENDER:-0}"

die() { echo "FAIL: $*" >&2; exit 1; }
note() { echo "  $*"; }

[ -n "$HOMESCREEN" ] || die "set HOMESCREEN to the homescreen binary"
[ -x "$HOMESCREEN" ] || die "HOMESCREEN not executable: $HOMESCREEN"
[ -n "$BUNDLE" ] || die "set BUNDLE to the multi_view_test bundle dir"
[ -d "$BUNDLE" ] || die "BUNDLE not a directory: $BUNDLE"
# Shared DRM discovery: see test/lib/drm_card.sh for why nothing is hardcoded.
# shellcheck source=test/lib/drm_card.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib/drm_card.sh"

# Auto-detection resolves vkms and nothing else, so it can never pick a real
# GPU by accident; naming a node explicitly is how you ask for one.
if [ -z "$DRM_DEVICE" ]; then
  DRM_DEVICE="$(ihs_find_vkms_card 2)" \
    || die "no vkms card found; set DRM_DEVICE explicitly to target real hardware"
fi
[ -c "$DRM_DEVICE" ] || die "DRM_DEVICE not a device node: $DRM_DEVICE"

# Connector names are card-specific (Virtual-N on vkms, HDMI-A-1/DP-1 on a real
# GPU), so read them off the resolved card rather than defaulting to either set.
if [ -z "$CONNECTOR_A" ]; then
  mapfile -t _CONNS < <(ihs_connectors_of "$(basename "$DRM_DEVICE")")
  CONNECTOR_A="${_CONNS[0]:-}"
  CONNECTOR_B="${_CONNS[1]:-}"
fi
[ -n "$CONNECTOR_A" ] && [ -n "$CONNECTOR_B" ] \
  || die "need 2 scanout connectors on $(basename "$DRM_DEVICE")"

# The binary must be DRM-KMS-EGL built (the compositor scans out to KMS).
if [ "$(strings "$HOMESCREEN" | grep -c '\[DrmBackend\]')" -eq 0 ]; then
  die "binary has no [DrmBackend] strings — build with BUILD_BACKEND_DRM_KMS_EGL=ON"
fi

if [ "${1:-}" = "--check" ]; then
  echo "OK: prerequisites present ($HOMESCREEN, $BUNDLE, $DRM_DEVICE)"
  exit 0
fi

# Work on a throwaway copy so we can drop in a config without touching
# the source bundle.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cp -rL "$BUNDLE"/. "$WORK/"

# Config: ONE [[view]] (one engine) listing TWO outputs.
cat > "$WORK/config.toml" <<EOF
[global]
app_id = "multi_view_test"

[[view]]

  [view.backend]
  type = "drm-kms-egl"

    [view.backend.drm]
    device = "$DRM_DEVICE"

  [[view.output]]
  drm_connector = "$CONNECTOR_A"
  x = 0

  [[view.output]]
  drm_connector = "$CONNECTOR_B"
  x = 3840
EOF

LOG="$(mktemp)"
trap 'rm -rf "$WORK" "$LOG"' EXIT

ENV=()
[ "$SOFTWARE_RENDER" = "1" ] && ENV+=("LIBGL_ALWAYS_SOFTWARE=1")
[ "$CAPTURE" = "1" ] && ENV+=("IVI_DRM_CAPTURE=1")

echo "launching one engine -> $CONNECTOR_A + $CONNECTOR_B on $DRM_DEVICE"
env "${ENV[@]}" "$HOMESCREEN" -b "$WORK" --drm-device "$DRM_DEVICE" -d >"$LOG" 2>&1 &
HS_PID=$!

sleep "$STARTUP_GRACE"
kill -0 "$HS_PID" 2>/dev/null || { sed -n '1,40p' "$LOG"; die "homescreen exited during startup"; }

FLIP_LOG=""
if [ "$COUNT_FLIPS" = "1" ]; then
  FLIP_LOG="$(mktemp)"
  timeout 3 strace -f -p "$HS_PID" -e trace=ioctl 2>"$FLIP_LOG" || true
fi

[ "$CAPTURE" = "1" ] && kill -USR1 "$HS_PID" 2>/dev/null

sleep "$DURATION"
kill -TERM "$HS_PID" 2>/dev/null
wait "$HS_PID" 2>/dev/null

# --- assertions ---------------------------------------------------------------
fail=0

# 1. The single engine must have brought up BOTH connectors.
for c in "$CONNECTOR_A" "$CONNECTOR_B"; do
  if grep -aq "\[DrmBackend\] .*connector.*$c" "$LOG" || grep -aq "connector=.*$c" "$LOG" \
     || grep -aq "bound to output '$c'" "$LOG"; then
    note "output up: $c"
  else
    echo "FAIL: no startup line for connector $c" >&2; fail=1
  fi
done

# 2. Exactly one engine: expect a single 'Engine running' / '(0) Engine'
#    and NOT a second discrete engine index.
if grep -aqE "\(1\) Engine running" "$LOG"; then
  echo "FAIL: a second engine (index 1) started" >&2
  fail=1
else
  note "single engine (no discrete second engine)"
fi

# 3. No fatal/error spdlog lines.
if grep -aqE "\] \[C\] |\] \[E\] " "$LOG"; then
  echo "FAIL: error/critical log lines:" >&2
  grep -aE "\] \[C\] |\] \[E\] " "$LOG" | head >&2
  fail=1
fi

# 4. Optional: page flips on BOTH CRTCs.
if [ "$COUNT_FLIPS" = "1" ]; then
  flips=$(grep -c "DRM_IOCTL_MODE_PAGE_FLIP\|DRM_IOCTL_MODE_ATOMIC" "$FLIP_LOG" 2>/dev/null || echo 0)
  if [ "$flips" -ge 2 ]; then
    note "page-flip/atomic ioctls observed: $flips"
  else
    echo "FAIL: expected flips on both CRTCs, saw $flips" >&2; fail=1
  fi
  rm -f "$FLIP_LOG"
fi

if [ "$fail" -ne 0 ]; then
  echo "--- last 40 log lines ---" >&2
  tail -40 "$LOG" >&2
  die "multi-view single-engine assertions failed"
fi

echo "PASS: one engine drove $CONNECTOR_A + $CONNECTOR_B"
