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
#   CENSUS_CARD /dev/dri/cardN of a real display to census instead of vkms.
#               Setting it drops the vkms requirement and skips detection: the
#               module check exists to make auto-detection meaningful, and an
#               explicitly named card needs neither. Board captures are the only
#               admissible source for the timing figures this harness records
#               but does not gate, so this is how those get produced in the same
#               census format as the vkms counts.
#   CENSUS_CONNECTOR
#               connector to pin (--drm-connector), e.g. DP-4. Unset lets the
#               backend pick. A board with several heads wired needs this or the
#               census may measure whichever one the backend chose that boot.
#   SOFTWARE_GL 1 = force llvmpipe (LIBGL_ALWAYS_SOFTWARE=1), the default,
#               because vkms has no render node behind it. Set 0 to census a
#               real GPU: left at 1 on a board it measures software GL and the
#               numbers say nothing about the hardware present path.
#   BACKENDS    space-separated list (default: "drm-kms-egl software")
#   CENSUS_SECS steady-state capture seconds per backend (default 8), counted
#               from the backend's first present window, not from launch
#   STARTUP_SECS
#               seconds a backend may take to present its first window
#               (default 30). Startup is not steady state: on a shared runner
#               drm-kms-egl has taken 7 to 14 s to present its first window,
#               and counting that against CENSUS_SECS failed the frames floor
#               on runs that then presented at a steady 60 Hz.
#   COUNT_FDS   1 to also fail a backend whose open-fd count grows over its
#               steady-state window (default 0). Rides along because this
#               already holds a live shell for CENSUS_SECS; see
#               test/lib/fd_count.sh for why growth and not an absolute count.
#   FD_GROWTH_LIMIT
#               fds a backend may gain before COUNT_FDS fails it (default 16)
#   FD_GRACE    seconds to let the run settle before the fd baseline is taken
#               (default 4). Must be less than CENSUS_SECS or the check is
#               skipped. The grace is a floor, not the whole wait: the baseline
#               is then read until it stops climbing, because a count still
#               rising is not a baseline.
#   FD_SETTLE_STEP / FD_SETTLE_TRIES / FD_SETTLE_DELTA
#               seconds between baseline reads, how many to take at most, and
#               the jitter two reads may differ by and still count as settled
#               (defaults 1, 8, 2). The last read is the baseline whether or not
#               it settled -- see test/lib/fd_count.sh for why not giving up is
#               what keeps a real leak from being skipped.
#   BASELINE    baseline file for --check when no path argument is given
#   KEEP_LOG    1 = keep the per-backend homescreen logs on exit and say where
#               (default 0). Every census figure is folded out of those logs, so
#               a failed invariant is only explicable with them. The census line
#               itself is already echoed to stdout, so this is about the raw
#               profile windows behind it.
#
# Exit: 0 all census invariants held (and drift within tolerance for --check);
#       1 an invariant failed or a backend produced no census; 2 bad usage/setup.
set -u

HOMESCREEN="${HOMESCREEN:-}"
BUNDLE="${BUNDLE:-}"
VKMS_CARD="${VKMS_CARD:-}"
CENSUS_CARD="${CENSUS_CARD:-}"
CARD=""              # the node actually censused: CENSUS_CARD, else vkms
CENSUS_CONNECTOR="${CENSUS_CONNECTOR:-}"
SOFTWARE_GL="${SOFTWARE_GL:-1}"
BACKENDS="${BACKENDS:-drm-kms-egl software}"
CENSUS_SECS="${CENSUS_SECS:-8}"
STARTUP_SECS="${STARTUP_SECS:-30}"
BASELINE="${BASELINE:-}"
# Read under set -u, so it needs a default here rather than at the use site.
KEEP_LOG="${KEEP_LOG:-0}"

# Keep-up floor: fraction of presented frames that landed at 30Hz or faster
# (60Hz + 30Hz buckets). vkms drives a 60Hz virtual head; software present adds
# jitter that spills a few frames into the 30Hz bucket, but nothing should fall
# below 30Hz on any runner fast enough to host the census.
KEEPUP_MIN="${KEEPUP_MIN:-0.90}"
# Open-fd growth check, off by default: the census is a present-path census
# first, and this rides along only because it already holds a live shell at
# steady state for CENSUS_SECS. See test/lib/fd_count.sh for why growth rather
# than an absolute count.
COUNT_FDS="${COUNT_FDS:-0}"
FD_GROWTH_LIMIT="${FD_GROWTH_LIMIT:-16}"
FD_GRACE="${FD_GRACE:-4}"
FD_SETTLE_STEP="${FD_SETTLE_STEP:-1}"
FD_SETTLE_TRIES="${FD_SETTLE_TRIES:-8}"
FD_SETTLE_DELTA="${FD_SETTLE_DELTA:-2}"

MODE="selfcheck"     # selfcheck | write | check
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMPDIR="$(mktemp -d)"
CENSUS_OUT="${TMPDIR}/census.txt"
PASS=0
FAIL=0
declare -a RESULTS=()

# No rm here: the EXIT trap below owns cleanup, and it is armed before any die()
# can run. Removing TMPDIR here as well would defeat KEEP_LOG -- including on the
# --write / --check failures, which happen after the census has already run.
die() { echo "error: $*" >&2; exit 2; }
log() { echo "[census] $*"; }
# KEEP_LOG=1 leaves the per-backend logs behind, and says where.
#
# Every census figure is folded out of those logs, so when an invariant fails
# they are the only thing that says why. Nothing kept here is bulky: the logs
# plus census.txt, which is echoed to stdout anyway.
#
# This is the only deletion site. die() and the --help branch used to remove
# TMPDIR themselves, which was already redundant -- the trap below is armed
# before either can run -- and would have defeated KEEP_LOG on exactly the paths
# where the logs matter most, since die() is reachable after the census has run,
# from --write and --check.
cleanup() {
    [[ -n "${HOMESCREEN:-}" ]] && pkill -KILL -f "$HOMESCREEN" 2>/dev/null
    if [[ "$KEEP_LOG" == "1" ]]; then
        log "logs kept: $TMPDIR"
    else
        rm -rf "$TMPDIR"
    fi
    return 0
}
trap cleanup EXIT

# ─── Argument parsing ────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --write)  MODE="write"; BASELINE="${2:-}"; shift 2 || die "--write needs a path" ;;
        --check)  MODE="check"; [[ -n "${2:-}" && "$2" != -* ]] && { BASELINE="$2"; shift; }; shift ;;
        # Derived, not a hardcoded range: the header block grows, and a fixed
        # last line silently truncated --help mid-sentence and dropped the
        # Exit: line once already. No rm here: the EXIT trap is already armed
        # and owns cleanup, and removing TMPDIR here would defeat KEEP_LOG.
        -h|--help) awk 'NR==1 { next } /^#/ { print; next } { exit }' \
                       "${BASH_SOURCE[0]}"; exit 0 ;;
        *) die "unknown argument '$1'" ;;
    esac
done
[[ "$MODE" == "write" && -z "$BASELINE" ]] && die "--write needs a baseline path"
[[ "$MODE" == "check" && -z "$BASELINE" ]] && die "--check needs a baseline path (arg or BASELINE=)"

# ─── Prerequisites ───────────────────────────────────────────────────────────

# Shared discovery rather than a local copy. The copy that used to live here
# matched "cardN-Virtual-*" and stopped at the first hit, which is ambiguous on
# a CI runner: Hyper-V's synthetic display uses that connector name too, so the
# search could return the VM's own display instead of vkms. That is not just a
# mislabeled log line -- VKMS_CARD is interpolated into
# IVI_SW_SINK="drm-dumb:${CARD}" below, so a wrong card points the software
# backend's dumb-buffer sink at the wrong DRM node and the census measures it.
# shellcheck source=test/lib/drm_card.sh
source "${ROOT_DIR}/test/lib/drm_card.sh"

# Open-fd accounting, shared with drm_kms_vkms.sh.
# shellcheck source=test/lib/fd_count.sh
source "${ROOT_DIR}/test/lib/fd_count.sh"

check_prereqs() {
    # CENSUS_CARD names a real display: no vkms module, no detection. The module
    # check below is there to make ihs_find_vkms_card's answer meaningful, and a
    # card given by name needs neither it nor the search.
    if [[ -n "$CENSUS_CARD" ]]; then
        CARD="$CENSUS_CARD"
        [[ -e "$CARD" ]] || die "CENSUS_CARD does not exist: $CARD"
    else
        [[ -d /sys/module/vkms ]] || die "vkms not loaded (sudo modprobe vkms)"
        [[ -z "$VKMS_CARD" ]] && VKMS_CARD="$(ihs_find_vkms_card)"
        [[ -n "$VKMS_CARD" && -e "$VKMS_CARD" ]] || die "no vkms /dev/dri/cardN found"
        CARD="$VKMS_CARD"
    fi
    [[ -n "$HOMESCREEN" && -x "$HOMESCREEN" ]] || die "HOMESCREEN must be an executable binary"
    [[ -n "$BUNDLE" && -d "$BUNDLE" ]] || die "BUNDLE must be a bundle directory"
    local conn="any connector"
    [[ -n "$CENSUS_CONNECTOR" ]] && conn="connector $CENSUS_CONNECTOR"
    log "card: $CARD ($conn) ; backends: $BACKENDS ; ${CENSUS_SECS}s/backend ;" \
        "software GL: $SOFTWARE_GL"
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
# actually presented, not the vsync-delivery pacing beside it). Named one by
# one rather than defaulted: the default used to answer "DrmVsync" for every
# non-software backend, which is the EGL backend's label alone. On
# drm-kms-vulkan the present stream is "VulkanDrmBackend" and "DrmVkVsync" is
# the cadence beside it, so the fold matched nothing and the backend reported
# "did not render" while it was presenting. A backend with no stream in this
# format is a setup error, not a silent empty census -- wayland-egl posts
# "swap profile", which the fold does not match.
#
# One present path is still uncounted: drm-kms-vulkan's plane path
# (PresentLayersViaPlanes) commits without recording, so a frame stream that
# places on planes censuses as zero frames rather than as itself.
present_label() {
    case "$1" in
        software)        echo "SoftwareBackend" ;;
        drm-kms-egl)     echo "DrmVsync" ;;
        drm-kms-vulkan)  echo "VulkanDrmBackend" ;;
        wayland-vulkan)  echo "WaylandVulkanBackend" ;;
        *)               return 1 ;;
    esac
}

# Wait until $1 holds at least $3 present windows for label $2, giving up after
# $4 seconds (rc 1).
#
# This is the "startup is over" signal, and it has to come from the workload
# rather than from the fd count. A count reads flat both when the shell has
# finished opening lazily and when it has not begun, and those are
# indistinguishable from the outside: a baseline of 8 was taken on a slow runner
# against a settled ~58, so ordinary opening afterwards read as fds+32 and
# failed a backend that leaks nothing. Two windows means the startup transient
# the fold already drops has passed and frames are flowing.
await_windows() {  # await_windows <log> <label> <count> <deadline_s>
    local log="$1" label="$2" want="$3" deadline="$4" waited=0 have
    while (( waited < deadline )); do
        have="$(grep -c "\[${label}\] profile (n=" "$log" 2>/dev/null)" || have=0
        (( have >= want )) && return 0
        sleep 1
        waited=$(( waited + 1 ))
    done
    return 1
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
    # Before the launch: a backend whose present stream this harness cannot
    # name is a setup error, and launching it would only produce an empty fold.
    local label
    if ! label="$(present_label "$backend")"; then
        record "$backend" skip "no present-census label known for this backend"
        return
    fi
    reap_homescreen
    local hs_log="${TMPDIR}/hs_${backend}.log"
    local extra_env=()
    [[ "$backend" == software ]] && extra_env+=(IVI_SW_SINK="drm-dumb:${CARD}")
    # vkms has no render node, so the default forces llvmpipe. On a real GPU
    # that would be what the census measured, hence the opt-out.
    [[ "$SOFTWARE_GL" == "1" ]] && extra_env+=(LIBGL_ALWAYS_SOFTWARE=1)
    local pin=()
    [[ -n "$CENSUS_CONNECTOR" ]] && pin=(--drm-connector "$CENSUS_CONNECTOR")
    # ${arr[@]+...}: expanding an empty array is an unbound-variable error under
    # set -u on bash before 4.4, and a board can be on 3.2.
    env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET IVI_PROFILE=1 \
        ${extra_env[@]+"${extra_env[@]}"} \
        "$HOMESCREEN" --backend "$backend" -b "$BUNDLE" \
        --drm-device "$CARD" ${pin[@]+"${pin[@]}"} >"$hs_log" 2>&1 &
    local pid=$!
    # Start the steady-state clock at the first present window. A launch that
    # never presents still runs the full window below and fails or skips there.
    local t0="$SECONDS" started=0
    while (( SECONDS - t0 < STARTUP_SECS )) && kill -0 "$pid" 2>/dev/null; do
        if grep -q "\[${label}\] profile (n=" "$hs_log" 2>/dev/null; then
            started=1; break
        fi
        sleep 0.2
    done
    if (( started )); then
        log "$backend first present window $(( SECONDS - t0 ))s after launch"
    else
        log "$backend no present window $(( SECONDS - t0 ))s after launch"
    fi
    # Optional fd-growth check (COUNT_FDS=1). Split the steady-state sleep so
    # the baseline is taken once the run has settled rather than at launch, when
    # the engine and EGL are still opening lazily. Falls back to one sleep when
    # the window is too short to split.
    local fd_base="" fd_why=""
    if [[ "$COUNT_FDS" == "1" && "$CENSUS_SECS" -gt "$FD_GRACE" ]]; then
        sleep "$FD_GRACE"
        local settle_t0="$SECONDS" rest
        # Frames first, then flatness. Waiting for a steady present window is
        # what tells startup from a stalled launch; settle_fds then rides out the
        # jitter once the shell is actually working.
        if await_windows "$hs_log" "$label" 2 \
                         "$(( CENSUS_SECS - 2 ))"; then
            fd_base="$(settle_fds "$pid" "$FD_SETTLE_STEP" "$FD_SETTLE_TRIES" \
                                  "$FD_SETTLE_DELTA")" || fd_base=""
        else
            fd_base=""
            log "$backend no steady present window in $(( CENSUS_SECS - 2 ))s;" \
                "fd check skipped (the frames floor below covers this run)"
        fi
        [[ -n "$fd_base" ]] && log "$backend fd baseline ${fd_base} taken" \
            "$(( SECONDS - settle_t0 ))s after the grace"
        # Settling comes out of the steady-state window rather than on top of it,
        # so the census still runs for about CENSUS_SECS. Keep a floor: a leak
        # needs some frames to show, and a baseline read right before the final
        # one would pass anything.
        rest=$(( CENSUS_SECS - FD_GRACE - (SECONDS - settle_t0) ))
        (( rest < 2 )) && rest=2
        sleep "$rest"
    else
        sleep "$CENSUS_SECS"
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        reap_homescreen; record "$backend" skip "homescreen exited before census (no frames)"; return
    fi
    # Sampled before the TERM below: teardown closes fds and would mask a leak.
    if [[ -n "$fd_base" ]]; then
        local fd_final fd_growth
        if fd_final="$(count_fds "$pid")"; then
            fd_growth=$(( fd_final - fd_base ))
            log "$backend open fds: ${fd_base} -> ${fd_final} (growth ${fd_growth}, limit ${FD_GROWTH_LIMIT})"
            if [[ "$fd_growth" -gt "$FD_GROWTH_LIMIT" ]]; then
                fd_why=" fds+${fd_growth}(>${FD_GROWTH_LIMIT})"
            fi
        fi
    fi
    kill -TERM "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; reap_homescreen

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
    local why="${fd_why}"
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
for r in ${RESULTS[@]+"${RESULTS[@]}"}; do echo "  $r"; done
log "backends: ${#RESULTS[@]}  PASS=$PASS  FAIL=$FAIL"

# Fail on a broken invariant or (in --check) a gated regression.
{ [[ "$FAIL" -eq 0 ]] && [[ "$drift_rc" -eq 0 ]] && [[ "$PASS" -gt 0 ]]; } || exit 1
exit 0
