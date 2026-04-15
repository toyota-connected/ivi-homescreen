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

set -euo pipefail

HOMESCREEN="${HOMESCREEN:-./build/homescreen}"
BUNDLE="${BUNDLE:-}"
BACKEND="${BACKEND:-egl}"
RECORD="${RECORD:-0}"
DURATION="${DURATION:-5}"
WIDTH="${WIDTH:-1920}"
HEIGHT="${HEIGHT:-1080}"

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
trap 'rm -rf "$TMPDIR"' EXIT

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

cleanup_hs() {
    if kill -0 "$HS_PID" 2>/dev/null; then
        kill "$HS_PID" || true
        wait "$HS_PID" 2>/dev/null || true
    fi
}
trap 'cleanup_hs; rm -rf "$TMPDIR"' EXIT

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
