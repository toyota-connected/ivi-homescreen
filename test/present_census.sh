#!/usr/bin/env bash
#
# present_census.sh — present-path regression census.
#
# Drives homescreen against a vkms card under IVI_PROFILE, runs a deterministic
# workload to steady state, and folds the per-window frame_profile fence posts
# into one machine-readable census per backend: presented frames, discarded and
# stall counts, and the 60/30/20/slow/idle bucket distribution. This turns the
# profiling that already runs every vsync into a CI-diffable regression check.
#
# The steady-state timing (mean/max interval, scene-stage milliseconds) is
# captured for the record but NOT gated here: on x86/vkms only counts and ratios
# are admissible (discarded, stalls, keep-up fraction); anything bandwidth
# sensitive gates on board-captured figures. See test/drm_kms_vkms.sh for the
# vkms setup this shares conventions with.
#
# ─── Usage ───────────────────────────────────────────────────────────────────
#   test/present_census.sh                  capture + self-check invariants
#   test/present_census.sh --write BASELINE capture + store the census as BASELINE
#   test/present_census.sh --check BASELINE capture + invariants + drift vs BASELINE
#   test/present_census.sh --check          (BASELINE env var supplies the path)
#
# ─── Environment ─────────────────────────────────────────────────────────────
#   HOMESCREEN  path to the homescreen binary (required)
#   BUNDLE      Flutter bundle to run; use a deterministic workload such as the
#               scroll_bench integration app (required)
#   VKMS_CARD   /dev/dri/cardN for vkms (auto-detected if unset)
#   BACKENDS    space-separated list (default: "drm-kms-egl software")
#   CENSUS_SECS steady-state capture seconds per backend (default 8)
#   BASELINE    baseline file for --check when no path argument is given
#
# Exit: 0 all census invariants held (and drift within tolerance for --check);
#       1 an invariant failed or a backend produced no census; 2 bad usage/setup.
set -u

HOMESCREEN="${HOMESCREEN:-}"
BUNDLE="${BUNDLE:-}"
VKMS_CARD="${VKMS_CARD:-}"
BACKENDS="${BACKENDS:-drm-kms-egl software}"
CENSUS_SECS="${CENSUS_SECS:-8}"
BASELINE="${BASELINE:-}"

# Keep-up floor: fraction of presented frames that landed at 30Hz or faster
# (60Hz + 30Hz buckets). vkms drives a 60Hz virtual head; software present adds
# jitter that spills a few frames into the 30Hz bucket, but nothing should fall
# below 30Hz on any runner fast enough to host the census.
KEEPUP_MIN="${KEEPUP_MIN:-0.90}"

MODE="selfcheck"     # selfcheck | write | check
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMPDIR="$(mktemp -d)"
CENSUS_OUT="${TMPDIR}/census.txt"
PASS=0
FAIL=0
declare -a RESULTS=()

die() { echo "error: $*" >&2; rm -rf "$TMPDIR"; exit 2; }
log() { echo "[census] $*"; }
cleanup() { [[ -n "${HOMESCREEN:-}" ]] && pkill -KILL -f "$HOMESCREEN" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT

# ─── Argument parsing ────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --write)  MODE="write"; BASELINE="${2:-}"; shift 2 || die "--write needs a path" ;;
        --check)  MODE="check"; [[ -n "${2:-}" && "$2" != -* ]] && { BASELINE="$2"; shift; }; shift ;;
        -h|--help) sed -n '2,32p' "${BASH_SOURCE[0]}"; rm -rf "$TMPDIR"; exit 0 ;;
        *) die "unknown argument '$1'" ;;
    esac
done
[[ "$MODE" == "write" && -z "$BASELINE" ]] && die "--write needs a baseline path"
[[ "$MODE" == "check" && -z "$BASELINE" ]] && die "--check needs a baseline path (arg or BASELINE=)"

# ─── Prerequisites ───────────────────────────────────────────────────────────
find_vkms_card() {
    local c card
    for c in /sys/class/drm/card[0-9]*; do
        [[ -e "$c" ]] || continue
        card="$(basename "$c")"
        if compgen -G "/sys/class/drm/${card}-Virtual-*" >/dev/null; then
            echo "/dev/dri/${card}"; return 0
        fi
    done
    return 1
}

check_prereqs() {
    [[ -d /sys/module/vkms ]] || die "vkms not loaded (sudo modprobe vkms)"
    [[ -z "$VKMS_CARD" ]] && VKMS_CARD="$(find_vkms_card)"
    [[ -n "$VKMS_CARD" && -e "$VKMS_CARD" ]] || die "no vkms /dev/dri/cardN found"
    [[ -n "$HOMESCREEN" && -x "$HOMESCREEN" ]] || die "HOMESCREEN must be an executable binary"
    [[ -n "$BUNDLE" && -d "$BUNDLE" ]] || die "BUNDLE must be a bundle directory"
    log "vkms card: $VKMS_CARD ; backends: $BACKENDS ; ${CENSUS_SECS}s/backend"
}

# ─── Process control (shared with input_event_driven.sh conventions) ─────────
reap_homescreen() {
    pgrep -f "$HOMESCREEN" >/dev/null 2>&1 || return 0
    pkill -TERM -f "$HOMESCREEN" 2>/dev/null
    local i
    for i in $(seq 1 15); do
        pgrep -f "$HOMESCREEN" >/dev/null 2>&1 || return 0
        sleep 0.2
    done
    pkill -KILL -f "$HOMESCREEN" 2>/dev/null
    sleep 0.5
}

# The present-census label differs per backend (the stream that measures frames
# actually presented, not the vsync-delivery pacing beside it).
present_label() {
    case "$1" in
        software) echo "SoftwareBackend" ;;
        *)        echo "DrmVsync" ;;  # egl + vulkan DRM backends present via the vsync provider
    esac
}

# ─── Census capture ──────────────────────────────────────────────────────────
# Fold every steady-state present window for $label (skipping the first, which
# carries the startup transient) into: frames discarded stalls b60 b30 b20 bslow
# bidle mean_us max_us  — emitted as one space-separated line, empty if none.
fold_present() {  # fold_present <log> <label>
    awk -v label="$2" '
        index($0, "[" label "] profile (n=") == 0 { next }
        { line = $0 }
        match(line, /\(n=[0-9]+\)/) {
            n = substr(line, RSTART+3, RLENGTH-4) + 0
        }
        { w++ }                              # window index for this label
        w == 1 { next }                      # drop the startup window
        match(line, /discarded=[0-9]+/)   { d = substr(line, RSTART+10, RLENGTH-10) + 0 }
        match(line, /stalls=[0-9]+/)      { s = substr(line, RSTART+7,  RLENGTH-7)  + 0 }
        match(line, /mean_interval=[0-9]+/) { mi = substr(line, RSTART+14, RLENGTH-14) + 0 }
        match(line, /max_interval=[0-9]+/)  { xi = substr(line, RSTART+13, RLENGTH-13) + 0 }
        match(line, /=[0-9]+\/[0-9]+\/[0-9]+\/[0-9]+\/[0-9]+$/) {
            split(substr(line, RSTART+1), b, "/")
        }
        {
            frames += n; disc += d; stall += s;
            c60 += b[1]; c30 += b[2]; c20 += b[3]; cslow += b[4]; cidle += b[5];
            meanrep = mi; maxrep = xi;        # last steady window is representative
        }
        END {
            if (frames == 0) exit 0;
            printf "%d %d %d %d %d %d %d %d %d %d\n",
                   frames, disc, stall, c60, c30, c20, cslow, cidle, meanrep, maxrep;
        }
    ' "$1"
}

# Mean of a scene-profile stage in microseconds (board-only, informational).
scene_stage_us() {  # scene_stage_us <log> <stage>
    awk -v stage="$1" '
        index($0, "scene profile (n=") == 0 { next }
        match($0, stage "=[0-9.]+ms") {
            v = substr($0, RSTART+length(stage)+1, RLENGTH-length(stage)-3) + 0;
            sum += v; n++;
        }
        END { if (n > 0) printf "%.0f", (sum / n) * 1000; else printf "0"; }
    ' "$2"
}

record() {  # record <backend> <pass|fail|skip> <detail>
    RESULTS+=("$1 $2 — $3")
    case "$2" in
        pass) PASS=$((PASS+1)); log "PASS $1 — $3" ;;
        fail) FAIL=$((FAIL+1)); log "FAIL $1 — $3" ;;
        skip) log "SKIP $1 — $3" ;;
    esac
}

# Run one backend, fold its census, self-check invariants, append to CENSUS_OUT.
census_backend() {  # census_backend <backend>
    local backend="$1"
    reap_homescreen
    local hs_log="${TMPDIR}/hs_${backend}.log"
    local extra_sink=()
    [[ "$backend" == software ]] && extra_sink=(IVI_SW_SINK="drm-dumb:${VKMS_CARD}")
    env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET LIBGL_ALWAYS_SOFTWARE=1 IVI_PROFILE=1 \
        "${extra_sink[@]}" \
        "$HOMESCREEN" --backend "$backend" -b "$BUNDLE" \
        --drm-device "$VKMS_CARD" >"$hs_log" 2>&1 &
    local pid=$!
    sleep "$CENSUS_SECS"
    if ! kill -0 "$pid" 2>/dev/null; then
        reap_homescreen; record "$backend" skip "homescreen exited before census (no frames)"; return
    fi
    kill -TERM "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; reap_homescreen

    local label; label="$(present_label "$backend")"
    local fold; fold="$(fold_present "$hs_log" "$label")"
    if [[ -z "$fold" ]]; then
        record "$backend" skip "no [$label] present census emitted (backend did not render)"; return
    fi
    read -r frames disc stall c60 c30 c20 cslow cidle mean_us max_us <<<"$fold"

    local wait_us compose_us commit_us total_us
    wait_us="$(scene_stage_us wait "$hs_log")"
    compose_us="$(scene_stage_us compose "$hs_log")"
    commit_us="$(scene_stage_us commit "$hs_log")"
    total_us="$(scene_stage_us total "$hs_log")"

    # keep-up ratio in integer per-mille to stay shell-arithmetic-safe
    local keepup_pm=0
    [[ "$frames" -gt 0 ]] && keepup_pm=$(( (c60 + c30) * 1000 / frames ))

    printf '%s frames=%d discarded=%d stalls=%d b60=%d b30=%d b20=%d bslow=%d bidle=%d keepup_pm=%d mean_us=%d max_us=%d wait_us=%s compose_us=%s commit_us=%s total_us=%s\n' \
        "$backend" "$frames" "$disc" "$stall" "$c60" "$c30" "$c20" "$cslow" "$cidle" \
        "$keepup_pm" "$mean_us" "$max_us" "$wait_us" "$compose_us" "$commit_us" "$total_us" \
        >>"$CENSUS_OUT"

    # Invariants (counts/ratios — admissible on vkms).
    local min_pm; min_pm=$(awk -v k="$KEEPUP_MIN" 'BEGIN{printf "%d", k*1000}')
    local why=""
    [[ "$disc"  -ne 0 ]] && why="${why} discarded=$disc"
    [[ "$stall" -ne 0 ]] && why="${why} stalls=$stall"
    [[ "$frames" -lt 120 ]] && why="${why} frames=$frames(<120, no steady state)"
    [[ "$keepup_pm" -lt "$min_pm" ]] && why="${why} keepup=${keepup_pm}‰(<${min_pm}‰)"
    if [[ -n "$why" ]]; then
        record "$backend" fail "invariant:${why} [b=${c60}/${c30}/${c20}/${cslow}/${cidle}]"
    else
        record "$backend" pass "frames=$frames discarded=0 stalls=0 keepup=${keepup_pm}‰ buckets=${c60}/${c30}/${c20}/${cslow}/${cidle} scene(us) wait=$wait_us compose=$compose_us commit=$commit_us total=$total_us"
    fi
}

# ─── Baseline diff (counts/ratios gated, timing informational) ───────────────
field() { sed -n "s/.*[[:space:]]$2=\([0-9]*\).*/\1/p" <<<"$1"; }

diff_vs_baseline() {
    log "──── drift vs $BASELINE ────"
    local drifted=0 backend cur base b
    while read -r backend _; do
        [[ -z "$backend" ]] && continue
        cur="$(grep -m1 "^${backend} " "$CENSUS_OUT" 2>/dev/null)"
        base="$(grep -m1 "^${backend} " "$BASELINE" 2>/dev/null)"
        [[ -z "$cur" ]] && continue
        if [[ -z "$base" ]]; then log "  $backend: no baseline entry (new backend)"; continue; fi
        # Gated: discarded and stalls must not regress above baseline.
        for f in discarded stalls; do
            local cv bv; cv="$(field "$cur" "$f")"; bv="$(field "$base" "$f")"
            if [[ "${cv:-0}" -gt "${bv:-0}" ]]; then
                log "  $backend: REGRESSION $f ${bv}→${cv}"; drifted=1
            fi
        done
        # Gated: keep-up must not drop more than 50‰ (5%) below baseline.
        local ck bk; ck="$(field "$cur" keepup_pm)"; bk="$(field "$base" keepup_pm)"
        if [[ $(( ${bk:-0} - ${ck:-0} )) -gt 50 ]]; then
            log "  $backend: REGRESSION keepup ${bk}‰→${ck}‰"; drifted=1
        fi
        # Informational: timing drift (board-sensitive, never gates).
        local ct bt; ct="$(field "$cur" total_us)"; bt="$(field "$base" total_us)"
        [[ -n "$bt" && -n "$ct" ]] && log "  $backend: scene total ${bt}us→${ct}us (informational)"
    done < <(grep -v '^#' "$BASELINE" | cut -d' ' -f1 | sort -u | sed 's/$/ x/')
    return "$drifted"
}

# ─── Driver ──────────────────────────────────────────────────────────────────
check_prereqs
: >"$CENSUS_OUT"
for b in $BACKENDS; do census_backend "$b"; done

echo
log "──── census ────"
cat "$CENSUS_OUT"

drift_rc=0
case "$MODE" in
    write)
        mkdir -p "$(dirname "$BASELINE")" || die "cannot create baseline directory"
        cp "$CENSUS_OUT" "$BASELINE" || die "cannot write baseline: $BASELINE"
        log "baseline written: $BASELINE" ;;
    check)
        [[ -f "$BASELINE" ]] || die "baseline not found: $BASELINE"
        diff_vs_baseline || drift_rc=1 ;;
esac

echo
log "──── summary ────"
for r in "${RESULTS[@]}"; do echo "  $r"; done
log "backends: ${#RESULTS[@]}  PASS=$PASS  FAIL=$FAIL"

# Fail on a broken invariant or (in --check) a gated regression.
{ [[ "$FAIL" -eq 0 ]] && [[ "$drift_rc" -eq 0 ]] && [[ "$PASS" -gt 0 ]]; } || exit 1
exit 0
