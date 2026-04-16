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
#   2. A bundle copy is prepared with drm_device pinned to the vkms node.
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
#   VKMS_CARD     explicit /dev/dri/cardN to target. Default: first card
#                 whose driver is vkms.
#   VKMS_AUTOLOAD 1 to `sudo modprobe vkms` if not loaded. Default: 0.
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
VKMS_CARD="${VKMS_CARD:-}"
VKMS_AUTOLOAD="${VKMS_AUTOLOAD:-0}"
SOFTWARE_RENDER="${SOFTWARE_RENDER:-0}"

die() {
    echo "error: $*" >&2
    exit 2
}

log() {
    echo "==> $*"
}

find_vkms_card() {
    # Identify the vkms card by its connector signature. vkms always
    # advertises connectors named "cardN-Virtual-M"; real GPUs don't.
    # This is more reliable than matching the driver symlink, whose name
    # has changed over kernel versions (platform → faux_driver).
    local c card
    for c in /sys/class/drm/card[0-9]*; do
        # Skip the connector/encoder child nodes themselves.
        [[ -d "$c/device" ]] || continue
        card="$(basename "$c")"
        if compgen -G "/sys/class/drm/${card}-Virtual-*" >/dev/null; then
            echo "/dev/dri/${card}"
            return 0
        fi
    done
    return 1
}

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

# Non-destructively edit the copied config.toml to pin drm_device. Adds a
# [view] table if missing; replaces any existing drm_device line within
# [view]; otherwise appends it. awk does the parsing — sed is too blunt
# with nested tables.
pin_drm_device() {
    local cfg="$1" card="$2"
    awk -v card="$card" '
        BEGIN {
            in_view = 0
            wrote = 0
            saw_view = 0
        }
        /^\[view\]/ {
            print
            print "drm_device = \"" card "\""
            in_view = 1
            saw_view = 1
            wrote = 1
            next
        }
        /^\[/ {
            in_view = 0
            print
            next
        }
        in_view && /^[[:space:]]*drm_device[[:space:]]*=/ {
            # Drop any prior override; the injected line above wins.
            next
        }
        { print }
        END {
            if (!saw_view) {
                print ""
                print "[view]"
                print "drm_device = \"" card "\""
            }
        }
    ' "$cfg" > "$cfg.new"
    mv "$cfg.new" "$cfg"
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
    VKMS_CARD="$(find_vkms_card)" || die "no /dev/dri/cardN with driver=vkms found"
fi
[[ -e "$VKMS_CARD" ]] || die "$VKMS_CARD does not exist"

log "vkms card: $VKMS_CARD"

if [[ "$CHECK_ONLY" == 1 ]]; then
    log "--check passed; vkms is ready"
    exit 0
fi

# ─── Bundle preparation ──────────────────────────────────────────────────

TMPDIR="$(mktemp -d -t drm-vkms.XXXXXXXX)"
trap 'rm -rf "$TMPDIR"' EXIT

BUNDLE_COPY="$TMPDIR/bundle"
mkdir -p "$BUNDLE_COPY"
ensure_bundle_copy "$BUNDLE_COPY"
pin_drm_device "$BUNDLE_COPY/config.toml" "$VKMS_CARD"
log "prepared bundle copy at $BUNDLE_COPY"

# ─── Launch ──────────────────────────────────────────────────────────────

LOG="$TMPDIR/homescreen.log"

LAUNCH_ENV=()
if [[ "$SOFTWARE_RENDER" == "1" ]]; then
    LAUNCH_ENV=(LIBGL_ALWAYS_SOFTWARE=1)
    log "software rendering: LIBGL_ALWAYS_SOFTWARE=1"
fi

log "launching $HOMESCREEN -b $BUNDLE_COPY -d"
env "${LAUNCH_ENV[@]}" "$HOMESCREEN" -b "$BUNDLE_COPY" -d >"$LOG" 2>&1 &
HS_PID=$!

cleanup_hs() {
    if kill -0 "$HS_PID" 2>/dev/null; then
        kill -TERM "$HS_PID" || true
        # Give it a chance to land a pending page flip.
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
trap 'cleanup_hs; rm -rf "$TMPDIR"' EXIT

sleep "$STARTUP_GRACE"

if ! kill -0 "$HS_PID" 2>/dev/null; then
    echo "error: homescreen exited during startup grace; log follows:" >&2
    sed 's/^/  | /' "$LOG" >&2
    exit 1
fi

log "homescreen alive; PID=$HS_PID"

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

# ─── Clean shutdown ──────────────────────────────────────────────────────

cleanup_hs

# ─── Log scan ────────────────────────────────────────────────────────────

# Backend / compositor / seat errors the log explicitly emits at error or
# critical levels. spdlog renders the level as a single-letter tag in
# square brackets: [C] / [E] / [W] / [I] / [D] / [T]. Match [C] / [E]
# directly rather than the word "critical" — the fmt patterns never
# include the English level name.
#
# `-a` forces text mode: spdlog's console sink emits ANSI colour codes
# that trip grep's binary-file heuristic and suppress match output.
if grep -aE '\] \[[CE]\] ' "$LOG"; then
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
    echo "error: no [DrmBackend] startup line in log — initialisation silently"
    echo "       failed or the build is not DRM-enabled." >&2
    sed 's/^/  | /' "$LOG" >&2
    exit 1
fi

log "OK — completed ${DURATION}s run against ${VKMS_CARD} with no errors"
