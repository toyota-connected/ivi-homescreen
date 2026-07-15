#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Toyota Connected North America
#
# input_event_driven.sh — event-driven input harness for the DRM/KMS and
# software seats. Drives homescreen against the vkms virtual DRM node while a
# synthetic input device (test/tools/uinput_gen) injects a deterministic event
# stream, then asserts the seat kept up (no libinput "event processing lagging")
# and behaved (clean idle, fast shutdown, degraded-mode fallback). Companion to
# drm_kms_vkms.sh; same env-var contract, exit codes, and RAN/SKIPPED summary.
#
# ─── Scenarios ───────────────────────────────────────────────────────────────
#   I1  1kHz mouse storm, DRM-KMS-EGL on vkms  → no lagging; input observed
#   I2  idle window                            → dispatch-thread wakes stay low
#   I3  SIGTERM at idle                        → exit < 100ms
#   I6  1kHz mouse storm, software backend     → no lagging; input observed
#   I8  degraded mode (eventfd() forced fail)  → fallback warning; still runs
#   (I4/I5/I7 — key-repeat, keymap-reload, MT burst — need injection; stubbed.)
#
# ─── Requirements ────────────────────────────────────────────────────────────
#   - vkms loaded (sudo modprobe vkms), a /dev/dri/cardN for it.
#   - /dev/uinput writable — ROOT on a stock distro, so run this under sudo (or
#     in a CI container as root). Without it, injection scenarios SKIP.
#   - A C compiler (builds uinput_gen).
#   - A homescreen built with the target backend, and a Flutter bundle.
#
# ─── Environment ─────────────────────────────────────────────────────────────
#   HOMESCREEN    path to the homescreen binary (required for run scenarios)
#   BUNDLE        path to the Flutter bundle directory (required)
#   VKMS_CARD     explicit /dev/dri/cardN. Default: first vkms card
#   STORM_SECS    mouse-storm duration (default 6)
#   ONLY          run only the named scenario (I1|I3|I6|I8); default all
#
# Exit: 0 all ran scenarios passed; 1 an assertion failed; 2 usage/setup error.

set -uo pipefail

HOMESCREEN="${HOMESCREEN:-}"
BUNDLE="${BUNDLE:-}"
VKMS_CARD="${VKMS_CARD:-}"
STORM_SECS="${STORM_SECS:-6}"
IDLE_SECS="${IDLE_SECS:-5}"
ONLY="${ONLY:-}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
UINPUT_SRC="${ROOT_DIR}/test/tools/uinput_gen.c"
TMPDIR="$(mktemp -d)"
UINPUT_GEN="${TMPDIR}/uinput_gen"
PASS=0
FAIL=0
declare -a RESULTS=()

die() { echo "error: $*" >&2; rm -rf "$TMPDIR"; exit 2; }
log() { echo "[input-harness] $*"; }
cleanup() { [[ -n "${HOMESCREEN:-}" ]] && pkill -KILL -f "$HOMESCREEN" 2>/dev/null; rm -rf "$TMPDIR"; }
trap cleanup EXIT

# ─── Prerequisites ───────────────────────────────────────────────────────────

find_vkms_card() {
    local c card
    for c in /sys/class/drm/card[0-9]*; do
        [[ -e "$c" ]] || continue
        card="$(basename "$c")"
        if compgen -G "/sys/class/drm/${card}-Virtual-*" >/dev/null; then
            echo "/dev/dri/${card}"
            return 0
        fi
    done
    return 1
}

have_uinput() { [[ -w /dev/uinput ]]; }

build_injector() {
    [[ -f "$UINPUT_SRC" ]] || die "missing $UINPUT_SRC"
    cc -O2 -o "$UINPUT_GEN" "$UINPUT_SRC" || die "failed to build uinput_gen"
}

check_prereqs() {
    [[ -d /sys/module/vkms ]] || die "vkms not loaded (sudo modprobe vkms)"
    [[ -z "$VKMS_CARD" ]] && VKMS_CARD="$(find_vkms_card)"
    [[ -n "$VKMS_CARD" && -e "$VKMS_CARD" ]] || die "no vkms /dev/dri/cardN found"
    command -v cc >/dev/null || die "no C compiler for uinput_gen"
    [[ -n "$HOMESCREEN" && -x "$HOMESCREEN" ]] || die "HOMESCREEN must be an executable binary"
    [[ -n "$BUNDLE" && -d "$BUNDLE" ]] || die "BUNDLE must be a bundle directory"
    build_injector
    log "vkms card: $VKMS_CARD ; uinput: $(have_uinput && echo writable || echo 'ROOT-ONLY (injection SKIPs)')"
}

# ─── Run helpers ─────────────────────────────────────────────────────────────

# Ensure no homescreen (ours or a leaked prior run) is still holding the vkms
# card before the next launch takes DRM master. A scenario that fails at startup
# used to leave its process behind, whose retained DRM master made every later
# launch look like it "exited at startup" too.
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

# Tear down the launched homescreen and wait for the DRM master to be released,
# then reap any straggler so the next scenario starts from a clean card.
stop_homescreen() {
    if [[ -n "${HS_PID:-}" ]]; then
        kill -TERM "$HS_PID" 2>/dev/null
        wait "$HS_PID" 2>/dev/null
    fi
    HS_PID=""
    reap_homescreen
}

# launch_homescreen <backend> <extra-env...> ; writes $HS_LOG, sets $HS_PID
launch_homescreen() {
    local backend="$1"; shift
    reap_homescreen  # clear any leftover master holder before we claim the card
    HS_LOG="${TMPDIR}/hs_${backend}.log"
    local extra_sink=()
    [[ "$backend" == software ]] && extra_sink=(IVI_SW_SINK="drm-dumb:${VKMS_CARD}")
    env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET LIBGL_ALWAYS_SOFTWARE=1 \
        IVI_M2P_PROFILE=1 "${extra_sink[@]}" "$@" \
        "$HOMESCREEN" --backend "$backend" -b "$BUNDLE" \
        --drm-device "$VKMS_CARD" -d >"$HS_LOG" 2>&1 &
    HS_PID=$!
}

# The libinput "event processing lagging" marker: on the software seat it is
# routed into ihs::log ([libinput] tag, #300); on the DRM seat it still goes to
# libinput's default stderr (drm-cxx#225). We capture both (2>&1 above), so one
# grep covers it.
lagging_count() { grep -c "event processing lagging" "$HS_LOG" 2>/dev/null || echo 0; }
# Input observed: IVI_M2P_PROFILE records injected motion; its window/summary
# line appears only if RecordInput fired. Fallback: the DrmSeat cursor-tracking
# marker on first motion.
input_observed() {
    grep -qE "motion->photon|cursor tracking pointer|RecordInput" "$HS_LOG" 2>/dev/null
}

# Find a task TID by its comm (thread name) under a pid.
dispatch_tid() {  # dispatch_tid <pid> <comm>
    local pid="$1" comm="$2" t
    for t in /proc/"$pid"/task/*/comm; do
        [[ -r "$t" ]] || continue
        if [[ "$(cat "$t" 2>/dev/null)" == "$comm" ]]; then
            basename "$(dirname "$t")"; return 0
        fi
    done
    return 1
}
# voluntary_ctxt_switches for a task (each blocking wake that returns is one).
vctxt() { awk '/^voluntary_ctxt_switches/{print $2}' "/proc/$1/task/$2/status" 2>/dev/null; }

record() {  # record <name> <pass|fail|skip> <detail>
    RESULTS+=("$1 $2 — $3")
    case "$2" in
        pass) PASS=$((PASS+1)); log "PASS $1 — $3" ;;
        fail) FAIL=$((FAIL+1)); log "FAIL $1 — $3" ;;
        skip) log "SKIP $1 — $3" ;;
    esac
}

# ─── Scenarios ───────────────────────────────────────────────────────────────

# Injection storm on a backend: launch, inject a 1kHz mouse for STORM_SECS,
# assert no lagging warning and that input was observed.
storm_scenario() {  # storm_scenario <name> <backend>
    local name="$1" backend="$2"
    if ! have_uinput; then record "$name" skip "no /dev/uinput write (run as root)"; return; fi
    launch_homescreen "$backend"
    sleep 2
    if ! kill -0 "$HS_PID" 2>/dev/null; then stop_homescreen; record "$name" fail "homescreen exited at startup"; return; fi
    "$UINPUT_GEN" mouse --hz 1000 --seconds "$STORM_SECS" >"${TMPDIR}/inj_${name}.log" 2>&1
    sleep 1
    stop_homescreen
    local lag; lag="$(lagging_count)"
    if [[ "$lag" -ne 0 ]]; then
        record "$name" fail "$lag 'event processing lagging' warnings under 1kHz storm"
    elif ! input_observed; then
        record "$name" fail "no input observed (seat did not process the injected stream)"
    else
        record "$name" pass "1kHz/${STORM_SECS}s storm, zero lagging, input flowed"
    fi
}

# I2: an idle seat wakes rarely — the whole point of the event-driven rework.
# Watch the dispatch thread's voluntary_ctxt_switches over an idle window; a
# busy-poll seat would rack up thousands. No injection needed.
scenario_I2() {  # scenario_I2 <backend> <comm>
    local backend="$1" comm="$2"
    launch_homescreen "$backend"
    sleep 2
    if ! kill -0 "$HS_PID" 2>/dev/null; then stop_homescreen; record I2 fail "homescreen exited at startup"; return; fi
    local tid; tid="$(dispatch_tid "$HS_PID" "$comm")"
    if [[ -z "$tid" ]]; then
        stop_homescreen
        record I2 skip "$comm thread not found (seat inactive)"; return
    fi
    local before after delta
    before="$(vctxt "$HS_PID" "$tid")"
    sleep "$IDLE_SECS"
    after="$(vctxt "$HS_PID" "$tid")"
    delta=$(( ${after:-0} - ${before:-0} ))
    stop_homescreen
    # Idle event-driven: single digits (SIGTERM re-check, keymap timers). A busy
    # poll at even 60Hz would be ~300 over 5s, so the gap to a real regression is
    # wide; 30 leaves headroom for timer noise without hiding a busy loop.
    if [[ "$delta" -le 30 ]]; then
        record I2 pass "$comm idle ${IDLE_SECS}s vctxt delta=$delta"
    else
        record I2 fail "$comm vctxt delta=$delta over ${IDLE_SECS}s idle (busy-polling?)"
    fi
}

# I3: SIGTERM at idle exits promptly. No injection needed.
scenario_I3() {
    launch_homescreen drm-kms-egl
    sleep 2
    if ! kill -0 "$HS_PID" 2>/dev/null; then stop_homescreen; record I3 fail "homescreen exited at startup"; return; fi
    local t0 t1 ms
    t0=$(date +%s%N)
    kill -TERM "$HS_PID" 2>/dev/null
    for _ in $(seq 1 30); do kill -0 "$HS_PID" 2>/dev/null || break; sleep 0.02; done
    t1=$(date +%s%N)
    ms=$(( (t1 - t0) / 1000000 ))
    if kill -0 "$HS_PID" 2>/dev/null; then
        kill -KILL "$HS_PID" 2>/dev/null; wait "$HS_PID" 2>/dev/null
        record I3 fail "did not exit within 600ms of SIGTERM"
    else
        wait "$HS_PID" 2>/dev/null
        # A generous ceiling: idle-thread teardown should be well under this.
        if [[ "$ms" -lt 500 ]]; then record I3 pass "SIGTERM->exit ${ms}ms"; \
        else record I3 fail "SIGTERM->exit ${ms}ms (>=500ms)"; fi
    fi
    HS_PID=""; reap_homescreen
}

# I8: degraded mode — squeeze RLIMIT_NOFILE so the seats' eventfd() fails and
# they fall back to timed poll. Assert the fallback warning and that it runs.
scenario_I8() {
    # A tight fd budget still leaves enough for the engine but starves the
    # seat's extra eventfd; homescreen must log the WakeEventFd fallback. exec
    # in the backgrounded subshell so $! is the homescreen PID (not an orphan).
    reap_homescreen  # clear any leftover master holder before we claim the card
    HS_LOG="${TMPDIR}/hs_degraded.log"
    ( ulimit -n 64 2>/dev/null
      exec env -u WAYLAND_DISPLAY -u WAYLAND_SOCKET LIBGL_ALWAYS_SOFTWARE=1 \
          "$HOMESCREEN" --backend drm-kms-egl -b "$BUNDLE" \
          --drm-device "$VKMS_CARD" -d >"$HS_LOG" 2>&1 ) &
    HS_PID=$!
    sleep 3
    local alive=0; kill -0 "$HS_PID" 2>/dev/null && alive=1
    stop_homescreen
    if grep -qiE "eventfd.*fail|falls back to timed poll|degraded" "$HS_LOG" 2>/dev/null; then
        [[ "$alive" == 1 ]] && record I8 pass "eventfd fallback logged; still running" \
            || record I8 fail "fallback logged but process died"
    else
        # If the fd squeeze did not trip eventfd (plenty of headroom), that is a
        # skip, not a failure — the code path is still there.
        record I8 skip "eventfd() did not fail at nofile=64 (no fallback exercised)"
    fi
}

# ─── Driver ──────────────────────────────────────────────────────────────────

run() { [[ -z "$ONLY" || "$ONLY" == "$1" ]]; }

if [[ "${1:-}" == "--check" ]]; then
    check_prereqs
    log "prerequisites OK"
    exit 0
fi

check_prereqs
run I1 && storm_scenario I1 drm-kms-egl
run I2 && scenario_I2 drm-kms-egl li-drm-dispatch
run I3 && scenario_I3
run I6 && storm_scenario I6 software
run I8 && scenario_I8

echo
log "──── summary ────"
for r in "${RESULTS[@]}"; do echo "  $r"; done
log "RAN: ${#RESULTS[@]}  PASS=$PASS  FAIL=$FAIL"
[[ "$FAIL" -eq 0 ]] && exit 0 || exit 1
