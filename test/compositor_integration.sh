#!/usr/bin/env bash
# Copyright 2026 Toyota Connected North America
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Phase 5 live-Wayland integration check for the compositor path.
#
# Requires:
#   - weston with xdg-shell (or any Wayland compositor)
#   - A compositor-enabled build of homescreen (BUILD_COMPOSITOR=ON)
#   - A Flutter bundle whose AOT snapshot and icudtl.dat are accessible
#   - wf-recorder + ffprobe for frame-rate verification (optional)
#
# Environment:
#   HOMESCREEN   path to the homescreen binary (default: ./build/homescreen)
#   BUNDLE       path to the Flutter bundle (required)
#   BACKEND      "egl" or "vulkan" (default: egl)
#   RECORD       1 to capture a short video via wf-recorder (default: 0)
#   DURATION     seconds to let the app run (default: 5)
#   WIDTH/HEIGHT window size in pixels (default: 1920x1080)
#   KEEP_LOG     1 to keep the log (and any recording) on exit and say where
#                (default: 0). Both failure paths here quote the log -- the
#                early-exit dump and the compositor error scan -- but only the
#                matching lines, and the trap then deleted the rest.

set -euo pipefail

HOMESCREEN="${HOMESCREEN:-./build/homescreen}"
BUNDLE="${BUNDLE:-}"
BACKEND="${BACKEND:-egl}"
RECORD="${RECORD:-0}"
DURATION="${DURATION:-5}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"
# Read inside the EXIT trap, so it needs a default here: under `set -u` an
# unset variable there aborts the trap rather than the script, which is the
# one failure bash -n cannot see.
KEEP_LOG="${KEEP_LOG:-0}"

if [[ -z "$BUNDLE" ]]; then
    echo "error: BUNDLE env var must point to a Flutter bundle" >&2
    exit 2
fi

if [[ ! -x "$HOMESCREEN" ]]; then
    echo "error: homescreen binary not found at $HOMESCREEN" >&2
    exit 2
fi

if [[ -z "${WAYLAND_DISPLAY:-}" && -z "${XDG_RUNTIME_DIR:-}" ]]; then
    echo "error: no Wayland display; start weston or set WAYLAND_DISPLAY" >&2
    exit 2
fi

TMPDIR="$(mktemp -d)"

# Stop the launched shell. Defined before the trap that calls it: the trap is
# armed here, but HS_PID is only set once homescreen is launched below, so a
# failure in between would otherwise call a function that does not exist yet.
cleanup_hs() {
    if [[ -n "${HS_PID:-}" ]] && kill -0 "$HS_PID" 2>/dev/null; then
        kill "$HS_PID" || true
        wait "$HS_PID" 2>/dev/null || true
    fi
}

# One EXIT trap for the whole script, rather than a second one installed after
# launch. Bash keeps only the last trap, so two of them means a half-applied
# change silently wins -- and bash -n is perfectly happy with it.
#
# KEEP_LOG=1 keeps the log, and the recording when RECORD=1. Both failure paths
# below quote the log, but only the part that matched: the early-exit dump and
# the compositor error scan. The rest went out with the trap.
#
# Under `set -euo pipefail` a trap is easy to break: an unset variable aborts it,
# and a test that returns false becomes the function's exit status. Hence the
# ${VAR:-} guard above and the explicit `return 0`.
cleanup() {
    cleanup_hs
    if [[ "$KEEP_LOG" == "1" ]]; then
        # ${LOG:-} because the trap is armed before LOG is assigned, a few lines
        # below. That window is narrow but reachable -- an interrupt is enough --
        # and under `set -u` an unset variable here aborts the trap itself,
        # leaking TMPDIR and reporting nothing useful.
        [[ -n "${LOG:-}" ]] && echo "==> log kept: $LOG"
        [[ -s "${VIDEO:-}" ]] && echo "==> recording kept: $VIDEO"
    else
        rm -rf "$TMPDIR"
    fi
    return 0
}
trap cleanup EXIT

LOG="$TMPDIR/homescreen.log"
VIDEO="$TMPDIR/compositor_${BACKEND}.mp4"

echo "==> backend=$BACKEND window=${WIDTH}x${HEIGHT} bundle=$BUNDLE"
echo "==> log=$LOG"

"$HOMESCREEN" \
    -b "$BUNDLE" \
    -w "$WIDTH" \
    -h "$HEIGHT" \
    >"$LOG" 2>&1 &
HS_PID=$!

# Give the engine a moment to spin up.
sleep 2

if ! kill -0 "$HS_PID" 2>/dev/null; then
    echo "error: homescreen exited early; log follows:" >&2
    sed 's/^/  | /' "$LOG" >&2
    exit 1
fi

if [[ "$RECORD" == "1" ]]; then
    if ! command -v wf-recorder >/dev/null; then
        echo "warning: wf-recorder not installed; skipping capture" >&2
    else
        wf-recorder -f "$VIDEO" --duration "$DURATION" >/dev/null 2>&1 || true
        if [[ -s "$VIDEO" ]] && command -v ffprobe >/dev/null; then
            ffprobe -v error -select_streams v:0 \
                -show_entries stream=r_frame_rate,nb_read_frames \
                -count_frames "$VIDEO"
        fi
    fi
else
    sleep "$DURATION"
fi

# Scan the log for compositor errors: missing subsurfaces, failed
# backing-store creation, Vulkan validation errors.
if grep -Ei 'compositor:.*(error|critical)|Failed to create backing store|validation error' "$LOG"; then
    echo "error: compositor reported issues; see log above" >&2
    exit 1
fi

echo "==> OK"
