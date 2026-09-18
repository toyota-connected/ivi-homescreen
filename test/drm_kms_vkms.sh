#!/usr/bin/env bash
# Copyright 2026 Toyota Connected North America
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Headless integration harness for the DRM/KMS EGL backend.
#
# Runs a compositor-enabled `homescreen` binary against the vkms virtual DRM
# device (no physical display required) and sanity-checks the run:
#
#   1. vkms module is loaded and /dev/dri/cardN for it exists.
#   2. A bundle copy is prepared; the vkms node is selected via --drm-device.
#   3. homescreen is launched; after a startup grace period we confirm the
#      process is still alive.
#   4. Optional strace sampling counts DRM page-flip ioctls to verify that
#      presentation actually happened.
#   5. The log is scanned for backend / compositor errors after shutdown.
#
# Exit status is 0 on a clean run, 1 on a failed run, 2 on misuse.
#
# Requirements:
#   - Linux with CONFIG_DRM_VKMS and `vkms` module installable.
#   - The user must be in the `video` / `render` groups (udev rules may
#     rename them to seat-bound equivalents). `sudo modprobe vkms` may be
#     needed to load the module; the harness does not escalate on its own.
#   - A compositor-enabled homescreen build: BUILD_BACKEND_DRM_KMS_EGL=ON
#     and BUILD_COMPOSITOR=ON.
#   - A Flutter bundle (config.toml, data/, lib/). The bundle is not
#     modified — the harness makes a throwaway copy.
#
# Environment variables:
#   HOMESCREEN    path to the homescreen binary (required)
#   BUNDLE        path to the Flutter bundle directory (required)
#   DURATION      seconds to let the app run after startup (default: 5)
#   STARTUP_GRACE seconds to wait before confirming process liveness
#                 (default: 2)
#   COUNT_FLIPS   1 to sample page-flip ioctls via strace (default: 0).
#                 strace must be installed and, on systems with YAMA
#                 ptrace_scope > 0, this may need sudo to attach. See the
#                 ptrace_scope check below.
#   COUNT_FDS     1 to check the shell's open-fd count for growth across the
#                 steady-state run (default: 0). #593 leaked one sync_file per
#                 explicit-sync submit and surfaced only as a ~100 ms stall
#                 each time the fd table doubled, then as the 1024 soft limit
#                 about half a minute in -- nothing in the log, and no test
#                 reaches the code it lives in. Growth rather than an absolute
#                 count: the engine and EGL open fds lazily, so the baseline is
#                 taken after the startup grace, not at launch.
#   FD_GROWTH_LIMIT
#                 fds the count may grow by before COUNT_FDS fails
#                 (default: 16). A per-frame leak clears this in well under a
#                 second at 60 Hz; the slack absorbs whatever the engine still
#                 opens after the grace period.
#   VKMS_CARD     explicit /dev/dri/cardN to target. Default: first card
#                 whose driver is vkms.
#   VKMS_AUTOLOAD 1 to `sudo modprobe vkms` if not loaded. Default: 0.
#   KEEP_LOG      1 to keep the shell's log on exit and print its path
#                 (default: 0). The failure paths below already dump the log
#                 inline, but the two log-scan failures print only the lines
#                 that matched, never the context around them, and a run that
#                 passes keeps nothing at all -- including the strace capture
#                 behind the page-flip count and its "no page-flip ioctls"
#                 warning, which is deliberately non-fatal. This keeps both.
#   SOFTWARE_RENDER
#                 1 to export LIBGL_ALWAYS_SOFTWARE=1 before launching
#                 homescreen. vkms is KMS-only and has no user-space
#                 renderer, so EGL/GBM on the vkms node needs Mesa's
#                 llvmpipe software rasteriser to actually produce frames.
#                 Default: 0.
#
# Usage:
#   env HOMESCREEN=build/shell/homescreen BUNDLE=/path/to/bundle \
#       test/drm_kms_vkms.sh
#
#   # prerequisite check only, no homescreen launch:
#   test/drm_kms_vkms.sh --check

set -euo pipefail

CHECK_ONLY=0
if [[ "${1:-}" == "--check" ]]; then
    CHECK_ONLY=1
fi

HOMESCREEN="${HOMESCREEN:-}"
BUNDLE="${BUNDLE:-}"
DURATION="${DURATION:-5}"
STARTUP_GRACE="${STARTUP_GRACE:-2}"
COUNT_FLIPS="${COUNT_FLIPS:-0}"
COUNT_FDS="${COUNT_FDS:-0}"
FD_GROWTH_LIMIT="${FD_GROWTH_LIMIT:-16}"
VKMS_CARD="${VKMS_CARD:-}"
VKMS_AUTOLOAD="${VKMS_AUTOLOAD:-0}"
SOFTWARE_RENDER="${SOFTWARE_RENDER:-0}"
KEEP_LOG="${KEEP_LOG:-0}"

die() {
    echo "error: $*" >&2
    exit 2
}

log() {
    echo "==> $*"
}

# Card/connector discovery is shared with the other KMS harnesses.
# shellcheck source=test/lib/drm_card.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib/drm_card.sh"

# Open-fd accounting, shared with present_census.sh.
# shellcheck source=test/lib/fd_count.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib/fd_count.sh"

ensure_vkms_loaded() {
    if lsmod | awk '{print $1}' | grep -qx vkms; then
        return 0
    fi
    if [[ "$VKMS_AUTOLOAD" == "1" ]]; then
        log "loading vkms via sudo modprobe"
        sudo modprobe vkms || die "sudo modprobe vkms failed"
        # Give udev a moment to create the device node.
        sleep 1
        return 0
    fi
    die "vkms module not loaded. Run: sudo modprobe vkms (or set VKMS_AUTOLOAD=1)"
}

ensure_bundle_copy() {
    local dst="$1"
    # -L dereferences symlinks so relative symlinks (e.g., flutter_assets
    # → ../../build/flutter_assets) resolve to real files in the copy.
    cp -rL "$BUNDLE"/. "$dst/"
    [[ -f "$dst/config.toml" ]] || die "bundle has no config.toml"
}

check_ptrace_scope() {
    local scope
    if [[ -r /proc/sys/kernel/yama/ptrace_scope ]]; then
        scope="$(cat /proc/sys/kernel/yama/ptrace_scope)"
        if [[ "$scope" != "0" ]]; then
            echo "warning: YAMA ptrace_scope=$scope; strace may need sudo." >&2
        fi
    fi
}

# ─── Pre-flight ───────────────────────────────────────────────────────────

if [[ "$CHECK_ONLY" == 0 ]]; then
    [[ -n "$HOMESCREEN" ]] || die "HOMESCREEN env var must point to the homescreen binary"
    [[ -x "$HOMESCREEN" ]] || die "$HOMESCREEN is not executable"
    [[ -n "$BUNDLE" ]] || die "BUNDLE env var must point to a Flutter bundle directory"
    [[ -d "$BUNDLE" ]] || die "$BUNDLE is not a directory"

    # Guard against stale build dirs that got reconfigured to a different
    # backend (CLion / clangd tooling occasionally re-runs cmake with
    # defaults, flipping BUILD_BACKEND_*). "[DrmBackend]" is the log tag
    # the backend prefixes every message with — only present in a DRM
    # build's string table.
    #
    # Counting instead of `grep -q` avoids a SIGPIPE on the `strings`
    # producer under `set -o pipefail` once grep short-circuits on the
    # first match.
    DRM_TAG_COUNT="$(strings "$HOMESCREEN" 2>/dev/null \
        | grep -c '\[DrmBackend\]' || true)"
    if [[ "$DRM_TAG_COUNT" == 0 ]]; then
        die "$HOMESCREEN was not built with BUILD_BACKEND_DRM_KMS_EGL=ON"
    fi
fi

ensure_vkms_loaded

if [[ -z "$VKMS_CARD" ]]; then
    VKMS_CARD="$(ihs_find_vkms_card)" || die "no /dev/dri/cardN with driver=vkms found"
fi
[[ -e "$VKMS_CARD" ]] || die "$VKMS_CARD does not exist"

log "vkms card: $VKMS_CARD"

if [[ "$CHECK_ONLY" == 1 ]]; then
    log "--check passed; vkms is ready"
    exit 0
fi

# ─── Bundle preparation ──────────────────────────────────────────────────

TMPDIR="$(mktemp -d -t drm-vkms.XXXXXXXX)"

# Stop the launched shell, giving it a chance to land a pending page flip.
# Defined before the trap that calls it: the trap is armed here, but HS_PID is
# only set once the shell is launched further down, so a failure in between
# would otherwise call a function that does not exist yet.
cleanup_hs() {
    if kill -0 "$HS_PID" 2>/dev/null; then
        kill -TERM "$HS_PID" || true
        for _ in 1 2 3 4 5; do
            kill -0 "$HS_PID" 2>/dev/null || break
            sleep 0.2
        done
        if kill -0 "$HS_PID" 2>/dev/null; then
            kill -KILL "$HS_PID" || true
        fi
        wait "$HS_PID" 2>/dev/null || true
    fi
}

# One EXIT trap for the whole script. Everything this run writes lives inside
# TMPDIR -- the log, the strace capture, the bundle copy -- so keeping the log
# means removing TMPDIR's contents selectively rather than the directory.
#
# Under `set -euo pipefail` a trap is easy to break: an unset variable aborts
# it, and a `[[ ]]` that tests false becomes the function's exit status and
# trips -e on the way out. Hence the ${VAR:-} guards, the literal paths in the
# keep branch (rm -rf "" returns 1), and the explicit `return 0`. bash -n
# cannot see any of that, so both branches are exercised by hand.
cleanup() {
    [[ -n "${HS_PID:-}" ]] && cleanup_hs
    if [[ "$KEEP_LOG" == "1" && -f "${LOG:-}" ]]; then
        rm -rf "$TMPDIR/bundle"
        log "shell log kept: $LOG"
        [[ -f "$TMPDIR/strace.log" ]] && log "strace capture kept: $TMPDIR/strace.log"
    else
        rm -rf "$TMPDIR"
    fi
    return 0
}
trap cleanup EXIT

BUNDLE_COPY="$TMPDIR/bundle"
mkdir -p "$BUNDLE_COPY"
ensure_bundle_copy "$BUNDLE_COPY"
log "prepared bundle copy at $BUNDLE_COPY"

# ─── Launch ──────────────────────────────────────────────────────────────

LOG="$TMPDIR/homescreen.log"

LAUNCH_ENV=()
if [[ "$SOFTWARE_RENDER" == "1" ]]; then
    LAUNCH_ENV=(LIBGL_ALWAYS_SOFTWARE=1)
    log "software rendering: LIBGL_ALWAYS_SOFTWARE=1"
fi

# Backend selection. On a bare VT the env-default picks the DRM backend, but
# when the harness is run from inside a Wayland session (developer desktop
# against vkms) the auto-selector sees WAYLAND_DISPLAY and picks wayland-egl,
# ignoring --drm-device. Always request the DRM backend explicitly, and unset
# the compositor socket so the run targets the vkms node rather than the host
# compositor.
if [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
    LAUNCH_ENV=(-u WAYLAND_DISPLAY -u WAYLAND_SOCKET "${LAUNCH_ENV[@]}")
    log "Wayland session detected; forcing --backend drm-kms-egl, unsetting WAYLAND_DISPLAY"
fi

log "launching $HOMESCREEN --backend drm-kms-egl -b $BUNDLE_COPY --drm-device $VKMS_CARD -d"
env "${LAUNCH_ENV[@]}" "$HOMESCREEN" --backend drm-kms-egl -b "$BUNDLE_COPY" \
    --drm-device "$VKMS_CARD" -d >"$LOG" 2>&1 &
HS_PID=$!

sleep "$STARTUP_GRACE"

if ! kill -0 "$HS_PID" 2>/dev/null; then
    echo "error: homescreen exited during startup grace; log follows:" >&2
    sed 's/^/  | /' "$LOG" >&2
    exit 1
fi

log "homescreen alive; PID=$HS_PID"

# ─── Optional: baseline the open-fd count ────────────────────────────────

FD_BASELINE=""
if [[ "$COUNT_FDS" == "1" ]]; then
    if FD_BASELINE="$(count_fds "$HS_PID")"; then
        log "open fds after startup grace: $FD_BASELINE"
    else
        echo "warning: cannot read /proc/$HS_PID/fd; skipping the fd check" >&2
        FD_BASELINE=""
    fi
fi

# ─── Optional: count page-flip ioctls via strace ─────────────────────────

if [[ "$COUNT_FLIPS" == "1" ]]; then
    check_ptrace_scope
    FLIP_LOG="$TMPDIR/strace.log"
    SAMPLE=2
    log "sampling DRM ioctls for ${SAMPLE}s via strace"
    # -e status=successful strips returns we don't care about; grep for
    # the page-flip ioctl name. The attach itself may fail silently on
    # restricted systems — that's why we sample into a separate log and
    # treat failures as non-fatal.
    (strace -p "$HS_PID" -e trace=ioctl -e status=successful -tt 2>"$FLIP_LOG" &
        STRACE_PID=$!
        sleep "$SAMPLE"
        kill "$STRACE_PID" 2>/dev/null || true
        wait "$STRACE_PID" 2>/dev/null || true) || true
    FLIPS="$(grep -ac DRM_IOCTL_MODE_PAGE_FLIP "$FLIP_LOG" 2>/dev/null || echo 0)"
    log "page-flip ioctls observed in ${SAMPLE}s: $FLIPS"
    # Even one flip means the backend survived mode-set and got into the
    # flip loop. Zero at 60 FPS after 2s is a red flag.
    if [[ "$FLIPS" -lt 1 ]]; then
        echo "warning: no page-flip ioctls observed — either strace could not"
        echo "         attach, or the backend is stuck before the flip loop."
    fi
fi

# ─── Steady-state run ────────────────────────────────────────────────────

REMAINING=$(( DURATION - STARTUP_GRACE ))
if [[ "$REMAINING" -gt 0 ]]; then
    sleep "$REMAINING"
fi

if ! kill -0 "$HS_PID" 2>/dev/null; then
    echo "error: homescreen exited during steady-state run; log follows:" >&2
    sed 's/^/  | /' "$LOG" >&2
    exit 1
fi

# ─── Optional: fd growth over the run ────────────────────────────────────
#
# Sampled while the process is still up, before cleanup_hs: teardown closes
# things, which would mask exactly the growth this is looking for.

if [[ "$COUNT_FDS" == "1" && -n "$FD_BASELINE" ]]; then
    if FD_FINAL="$(count_fds "$HS_PID")"; then
        FD_GROWTH=$(( FD_FINAL - FD_BASELINE ))
        log "open fds after ${DURATION}s: $FD_FINAL (baseline $FD_BASELINE, growth $FD_GROWTH)"
        if [[ "$FD_GROWTH" -gt "$FD_GROWTH_LIMIT" ]]; then
            echo "error: open fds grew by $FD_GROWTH over the run (limit $FD_GROWTH_LIMIT);" >&2
            echo "       something is leaking one per frame or per submit. Log follows:" >&2
            sed 's/^/  | /' "$LOG" >&2
            exit 1
        fi
    else
        echo "warning: cannot read /proc/$HS_PID/fd at end of run; fd check incomplete" >&2
    fi
fi

# ─── Clean shutdown ──────────────────────────────────────────────────────

cleanup_hs

# ─── Log scan ────────────────────────────────────────────────────────────

# Backend / compositor / seat errors the log explicitly emits at error or
# critical levels. spdlog renders the level as a single-letter tag in
# square brackets: [C] / [E] / [W] / [I] / [D] / [T]. Match [C] / [E]
# directly rather than the word "critical" — the fmt patterns never
# include the English level name.
#
# Anchored to the leading timestamp ("04:01:53.161 [E] SHEL: ..."), not to a
# preceding "] ". Nothing in this format puts a bracket immediately before the
# level, so the older pattern matched no error line at all and reported a clean
# log on runs that carried them. The anchor also stops a message quoting a level
# in its own text from forging a failure.
#
# `-a` forces text mode: spdlog's console sink emits ANSI color codes
# that trip grep's binary-file heuristic and suppress match output.
if grep -aE '^[0-9][0-9:.]* \[[CE]\] ' "$LOG"; then
    echo "error: critical/error log entries detected; see above" >&2
    exit 1
fi
if grep -aE '\[(DrmBackend|DrmCompositor|DrmSeat)\] (error|warn)' "$LOG"; then
    echo "error: DRM backend reported errors/warnings; see above" >&2
    exit 1
fi

# Make sure we actually saw the startup line: this means InitDrm /
# InitGbm / InitEgl all succeeded and a mode was selected.
if ! grep -aq '\[DrmBackend\] connector=' "$LOG"; then
    echo "error: no [DrmBackend] startup line in log — initialization silently"
    echo "       failed or the build is not DRM-enabled." >&2
    sed 's/^/  | /' "$LOG" >&2
    exit 1
fi

log "OK — completed ${DURATION}s run against ${VKMS_CARD} with no errors"
