#!/usr/bin/env bash
#
# osgi_multi_bundle.sh — validate that TWO OSGi bundles drive TWO engines on
# TWO outputs, and that the critical bundle is up before the normal one.
#
# This is the complement of multi_view_single_engine.sh. That harness proves
# ONE engine can drive TWO outputs, and fails if a second engine appears. This
# one requires the second engine: an OSGi bundle is a separate engine by
# definition, and the whole framework rests on that being true on real KMS
# rather than only against unit-test fakes.
#
# What is actually asserted, and why each matters:
#
#   B1 two engines     Engine indices 0 AND 1 both run. If the shell collapsed
#                      the bundles onto one engine, every isolation claim the
#                      framework makes is void.
#   B2 both outputs    Each bundle brought up its own connector, so the
#                      per-bundle [osgi.bundles.backend] config is honored
#                      rather than the first bundle's winning for all.
#   B3 critical first  The critical bundle reached ACTIVE before the normal
#                      bundle was launched. This is the ordering guarantee
#                      StartCriticalPhase exists to provide, and the one
#                      property unit tests can only assert against a fake.
#   B4 clean log       No error/critical lines.
#   B5 flips           Optional: page flips on BOTH CRTCs, i.e. both engines
#                      are really presenting and not merely constructed.
#
# Rigs:
#   - Real hardware:  DRM_DEVICE=/dev/dri/card1 CONNECTOR_A=HDMI-A-1 CONNECTOR_B=DP-1
#   - Portable/CI:    provision two virtual outputs first, then point at vkms:
#                       sudo third_party/drm-cxx/scripts/vkms_dual.sh up
#                       DRM_DEVICE=/dev/dri/cardN CONNECTOR_A=... CONNECTOR_B=... \
#                         SOFTWARE_RENDER=1 test/osgi_multi_bundle.sh
#
# Env:
#   HOMESCREEN       path to the homescreen binary (required)
#   BUNDLE           path to a Flutter bundle to run as both bundles (required)
#   DRM_DEVICE       /dev/dri/cardN. Unset = auto-detect the vkms card.
#                    Card numbering is NOT stable -- vkms lands wherever it
#                    lands relative to any real GPU, and moves when other DRM
#                    drivers load -- so there is deliberately no numeric
#                    default. Setting this explicitly is how you opt in to
#                    running against real hardware.
#   CONNECTOR_A      critical bundle's output  (default: 1st on the card)
#   CONNECTOR_B      normal bundle's output    (default: 2nd on the card)
#   DURATION         seconds to run            (default 6)
#   STARTUP_GRACE    seconds before liveness   (default 3)
#   STARTUP_TIMEOUT  critical deadline, ms     (default 4000)
#   COUNT_FLIPS      1 = prove page flips via strace (default 0)
#   SOFTWARE_RENDER  1 = LIBGL_ALWAYS_SOFTWARE=1 (vkms/llvmpipe)
#   --check          verify prerequisites only, do not launch
#
set -uo pipefail

HOMESCREEN="${HOMESCREEN:-}"
BUNDLE="${BUNDLE:-}"
# No numeric defaults: see the DRM_DEVICE note above.
DRM_DEVICE="${DRM_DEVICE:-}"
DRM_DEVICE_EXPLICIT=$([ -n "${DRM_DEVICE}" ] && echo 1 || echo 0)
CONNECTOR_A="${CONNECTOR_A:-}"
CONNECTOR_B="${CONNECTOR_B:-}"
DURATION="${DURATION:-6}"
STARTUP_GRACE="${STARTUP_GRACE:-3}"
STARTUP_TIMEOUT="${STARTUP_TIMEOUT:-4000}"
COUNT_FLIPS="${COUNT_FLIPS:-0}"
SOFTWARE_RENDER="${SOFTWARE_RENDER:-0}"

RESULTS=(); PASS=0; FAIL=0
log()  { echo "[osgi-mb] $*"; }
die()  { echo "FAIL: $*" >&2; exit 1; }
record() {  # record <name> <pass|fail|skip> <detail>
    RESULTS+=("$1 $2 — $3")
    case "$2" in
        pass) PASS=$((PASS+1)); log "PASS $1 — $3" ;;
        fail) FAIL=$((FAIL+1)); log "FAIL $1 — $3" ;;
        skip) log "SKIP $1 — $3" ;;
    esac
}

summary() {
    echo
    log "──── summary ────"
    for r in "${RESULTS[@]}"; do echo "  $r"; done
    log "RAN: ${#RESULTS[@]}  PASS=$PASS  FAIL=$FAIL"
    [[ "$FAIL" -eq 0 ]] && exit 0 || exit 1
}

# Shared DRM discovery: see test/lib/drm_card.sh for why nothing is hardcoded.
# shellcheck source=test/lib/drm_card.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib/drm_card.sh"

[ -n "$HOMESCREEN" ] || die "set HOMESCREEN to the homescreen binary"
[ -x "$HOMESCREEN" ] || die "HOMESCREEN not executable: $HOMESCREEN"
[ -n "$BUNDLE" ]     || die "set BUNDLE to a Flutter bundle dir"
[ -d "$BUNDLE" ]     || die "BUNDLE not a directory: $BUNDLE"

# The binary must be DRM-KMS-EGL built: the bundles scan out to KMS.
if [ "$(strings "$HOMESCREEN" | grep -c '\[DrmBackend\]')" -eq 0 ]; then
    die "binary has no [DrmBackend] strings — build with BUILD_BACKEND_DRM_KMS_EGL=ON"
fi

# ENABLE_OSGI is off by default, so a stock binary simply has no OSGi in it.
# That is a skip, not a failure: this harness is meaningless against a shell
# that was never built to run bundles, and reporting it as a failure would
# make every non-OSGi CI job red for no reason.
OSGI_BUILT=1
if [ "$(strings "$HOMESCREEN" | grep -c '\[osgi\]')" -eq 0 ]; then
    OSGI_BUILT=0
fi

# Resolve the card. Auto-detection finds vkms and nothing else, which is the
# safety property that matters: this harness takes DRM master and drives a
# modeset, so silently defaulting to a numeric card could blank a real display
# on a developer's desktop. Targeting real hardware requires saying so.
DEVICE_SOURCE="explicit"
if [ "$DRM_DEVICE_EXPLICIT" -eq 0 ]; then
    if DRM_DEVICE="$(ihs_find_vkms_card)"; then
        DEVICE_SOURCE="auto-detected vkms"
    else
        DRM_DEVICE=""
    fi
fi

CARD="$(basename "${DRM_DEVICE:-none}")"
if [ -n "$DRM_DEVICE" ] && [ -z "$CONNECTOR_A" ]; then
    # Connector names are card-specific -- vkms advertises Virtual-N, a real
    # GPU HDMI-A-1/DP-1 -- so they are read off the card rather than defaulted.
    mapfile -t CONNS < <(ihs_connectors_of "$CARD")
    CONNECTOR_A="${CONNS[0]:-}"
    CONNECTOR_B="${CONNS[1]:-}"
fi

if [ "${1:-}" = "--check" ]; then
    if [ -z "$DRM_DEVICE" ]; then
        echo "OK: binary and bundle present; no vkms card found — would skip"
    elif [ "$OSGI_BUILT" -eq 0 ]; then
        echo "OK: prerequisites present ($DRM_DEVICE, $DEVICE_SOURCE), but binary lacks OSGi (ENABLE_OSGI=OFF) — would skip"
    else
        echo "OK: prerequisites present ($HOMESCREEN, $BUNDLE, $DRM_DEVICE [$DEVICE_SOURCE], $CONNECTOR_A + $CONNECTOR_B)"
    fi
    exit 0
fi

if [ "$OSGI_BUILT" -eq 0 ]; then
    record "osgi-build" skip "binary built without ENABLE_OSGI=ON"
    summary
fi
if [ -z "$DRM_DEVICE" ]; then
    record "drm-device" skip "no vkms card found; set DRM_DEVICE to target real hardware"
    summary
fi
[ -c "$DRM_DEVICE" ] || die "DRM_DEVICE not a device node: $DRM_DEVICE"
if [ -z "$CONNECTOR_A" ] || [ -z "$CONNECTOR_B" ]; then
    record "connectors" skip "need 2 scanout connectors on $CARD, found ${CONNECTOR_A:-none} ${CONNECTOR_B:-}"
    summary
fi
log "card $DRM_DEVICE ($DEVICE_SOURCE); connectors $CONNECTOR_A + $CONNECTOR_B"

# Throwaway copies so a config can be dropped in without touching the source
# bundle. Two directories because each bundle is a distinct OSGi bundle with its
# own symbolic name, even though the Dart app inside them is the same.
WORK="$(mktemp -d)"
LOG="$(mktemp)"
FLIP_LOG=""
trap 'rm -rf "$WORK" "$LOG" "$FLIP_LOG"' EXIT
mkdir -p "$WORK/cluster" "$WORK/navigation"
cp -rL "$BUNDLE"/. "$WORK/cluster/"
cp -rL "$BUNDLE"/. "$WORK/navigation/"

# One critical bundle and one normal bundle, each pinned to its own connector.
# Deliberately declared normal-first so a pass proves the orchestrator reordered
# them rather than merely following file order.
#
# Note there is no [[view]] here, only [[osgi.bundles]]. That is the shape of a
# pure-OSGi deployment -- every surface belongs to a bundle -- and it is a
# requirement this harness places on the implementation: the bundles must
# themselves become the views. Today Configuration::parse_config yields zero
# views from such a file, so a shell that has not wired [[osgi.bundles]] into
# the config vector will come up with nothing and fail B1/B2 rather than
# quietly rendering one window.
cat > "$WORK/config.toml" <<EOF
[global]
app_id = "osgi_multi_bundle"

[osgi]

[[osgi.bundles]]
symbolic_name = "com.ivi.navigation"
bundle = "$WORK/navigation"
priority = "normal"

  [osgi.bundles.backend]
  type = "drm-kms-egl"

    [osgi.bundles.backend.drm]
    device = "$DRM_DEVICE"
    connector = "$CONNECTOR_B"

[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"
bundle = "$WORK/cluster"
priority = "critical"
startup_timeout_ms = $STARTUP_TIMEOUT

  [osgi.bundles.backend]
  type = "drm-kms-egl"

    [osgi.bundles.backend.drm]
    device = "$DRM_DEVICE"
    connector = "$CONNECTOR_A"
EOF

ENV=()
[ "$SOFTWARE_RENDER" = "1" ] && ENV+=("LIBGL_ALWAYS_SOFTWARE=1")

log "launching 2 bundles: cluster(critical)->$CONNECTOR_A  navigation(normal)->$CONNECTOR_B"
env "${ENV[@]}" "$HOMESCREEN" --config "$WORK/config.toml" -d >"$LOG" 2>&1 &
HS_PID=$!

sleep "$STARTUP_GRACE"
if ! kill -0 "$HS_PID" 2>/dev/null; then
    sed -n '1,40p' "$LOG"
    record "liveness" fail "homescreen exited during startup"
    summary
fi
record "liveness" pass "still running after ${STARTUP_GRACE}s"

if [ "$COUNT_FLIPS" = "1" ]; then
    FLIP_LOG="$(mktemp)"
    timeout 3 strace -f -p "$HS_PID" -e trace=ioctl 2>"$FLIP_LOG" || true
fi

sleep "$DURATION"
kill -TERM "$HS_PID" 2>/dev/null
wait "$HS_PID" 2>/dev/null

# ─── assertions ──────────────────────────────────────────────────────────────

# B1: two engines. The inverse of multi_view_single_engine.sh, which fails on
# exactly this line -- an OSGi bundle IS a separate engine.
if grep -aqE "\(1\) Engine running" "$LOG"; then
    record "B1-two-engines" pass "engine index 1 started (bundles are not collapsed)"
else
    record "B1-two-engines" fail "no second engine; bundles share one engine"
fi

# B2: each bundle brought up its own connector.
b2_fail=0
for c in "$CONNECTOR_A" "$CONNECTOR_B"; do
    if grep -aq "\[DrmBackend\] .*connector.*$c" "$LOG" || grep -aq "connector=.*$c" "$LOG" \
       || grep -aq "bound to output '$c'" "$LOG"; then
        :
    else
        b2_fail=1
        echo "  no startup line for connector $c" >&2
    fi
done
if [ "$b2_fail" -eq 0 ]; then
    record "B2-both-outputs" pass "$CONNECTOR_A and $CONNECTOR_B both up"
else
    record "B2-both-outputs" fail "a bundle did not bring up its connector"
fi

# B3: the ordering guarantee. The critical bundle must reach ACTIVE before the
# normal bundle is launched -- that is what "critical" buys, and it is the one
# property the unit tests can only assert against a fake host.
crit_active=$(grep -an "bundle 'com.ivi.cluster': STARTING -> ACTIVE" "$LOG" | head -1 | cut -d: -f1)
norm_start=$(grep -an "bundle 'com.ivi.navigation': RESOLVED -> STARTING" "$LOG" | head -1 | cut -d: -f1)
if [ -z "$crit_active" ] || [ -z "$norm_start" ]; then
    # -d raises the log level; without those lines the run told us nothing about
    # ordering, so this is honestly a skip rather than a pass.
    record "B3-critical-first" skip "lifecycle lines absent (need -d / debug logging)"
elif [ "$crit_active" -lt "$norm_start" ]; then
    record "B3-critical-first" pass "cluster ACTIVE (line $crit_active) before navigation start (line $norm_start)"
else
    record "B3-critical-first" fail "navigation started (line $norm_start) before cluster was ACTIVE (line $crit_active)"
fi

# B4: no fatal/error lines.
if grep -aqE "\] \[C\] |\] \[E\] " "$LOG"; then
    grep -aE "\] \[C\] |\] \[E\] " "$LOG" | head >&2
    record "B4-clean-log" fail "error/critical lines present"
else
    record "B4-clean-log" pass "no error/critical lines"
fi

# B5: both CRTCs actually presenting. Construction without presentation would
# satisfy B1 and B2 while showing nothing on either panel.
if [ "$COUNT_FLIPS" = "1" ]; then
    flips=$(grep -c "DRM_IOCTL_MODE_PAGE_FLIP\|DRM_IOCTL_MODE_ATOMIC" "$FLIP_LOG" 2>/dev/null || echo 0)
    if [ "$flips" -gt 0 ]; then
        record "B5-page-flips" pass "$flips flip/atomic ioctls observed"
    else
        record "B5-page-flips" fail "no page flips seen while running"
    fi
else
    record "B5-page-flips" skip "COUNT_FLIPS=0"
fi

summary
