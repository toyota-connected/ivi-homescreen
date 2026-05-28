#!/usr/bin/env bash
# Copyright 2026 Toyota Connected North America
#
# Assembles a Pi-ready Flutter bundle from the current Flutter app
# directory. Run from the root of a Flutter project (where pubspec.yaml
# lives).
#
# Two paths:
#
#   1. WITHOUT FLUTTER_WORKSPACE — the bundle is debug-mode only
#      (kernel_blob.bin / JIT). Wonderous and other GPU-bound apps
#      may fail to render on Pi-class hardware in debug. Useful for
#      quick smoke / desktop testing only. The script warns loudly.
#
#   2. WITH FLUTTER_WORKSPACE set — invokes
#        $FLUTTER_WORKSPACE/flutter_workspace.py --create-aot
#      with QEMU_LD_PREFIX + GEN_SNAPSHOT pre-configured to cross-
#      compile Dart to aarch64 AOT via the workspace's prebuilt
#      arm64 gen_snapshot. Stages the produced libapp.so.release
#      into <bundle>/lib/libapp.so. The resulting bundle is
#      Runtime=release and renders reliably under DRM/KMS.
#
# Prerequisites (both paths):
#   - `flutter build bundle` must have been run (or `flutter run` which
#     does it implicitly). The script verifies build/flutter_assets exists.
#   - Flutter engine artifacts (libflutter_engine.so, icudtl.dat) at
#     $ENGINE_BUNDLE (defaults under FLUTTER_WORKSPACE).
#
# Prerequisites (AOT path only):
#   - FLUTTER_WORKSPACE points at a workspace-automation checkout with:
#       flutter_workspace.py
#       create_aot.py
#       flutter/engine/src/flutter/prebuilts/linux-arm64/dart-sdk/bin/utils/gen_snapshot
#   - Cross-build sysroot at $XC_SYSROOT (defaults to
#     $XDG_CACHE_HOME/ivi-homescreen-xc/sysroot/$PIOS, PIOS=trixie —
#     same sysroot build_pi.sh creates).
#
# Usage:
#   cd /path/to/flutter/app
#   flutter build bundle                                # debug
#   /path/to/ivi-homescreen/scripts/build_pi_bundle.sh [/dev/dri/cardN]
#
#   # Or for the AOT / release bundle (recommended for Pi):
#   export FLUTTER_WORKSPACE=/mnt/raid10/workspace-automation
#   /path/to/ivi-homescreen/scripts/build_pi_bundle.sh [/dev/dri/cardN]
#
# Env overrides:
#   PIOS              sysroot variant: bookworm|trixie (default trixie)
#   XC_SYSROOT        explicit sysroot path (overrides $PIOS-derived)
#   GEN_SNAPSHOT      explicit gen_snapshot path
#   ENGINE_BUNDLE     path with lib/libflutter_engine.so + data/icudtl.dat
#                     (skips FLUTTER_WORKSPACE derivation)
#   CONFIG_TEMPLATE   path to a config.toml to seed the bundle's config

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

die()  { echo "error: $*" >&2; exit 1; }
log()  { echo "==> $*"; }

# ── Configurable paths ───────────────────────────────────────────────────

PIOS="${PIOS:-trixie}"
XC_ROOT="${IVI_XC_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/ivi-homescreen-xc}"
XC_SYSROOT="${XC_SYSROOT:-$XC_ROOT/sysroot/$PIOS}"

# ENGINE_BUNDLE may also be set directly (skips the FLUTTER_WORKSPACE check).
if [[ -z "${ENGINE_BUNDLE:-}" ]]; then
    [[ -n "${FLUTTER_WORKSPACE:-}" ]] \
        || die "set FLUTTER_WORKSPACE or ENGINE_BUNDLE before running"
    ENGINE_BUNDLE="${FLUTTER_WORKSPACE}/flutter-engine/bundle-debug-x64"
fi
CONFIG_TEMPLATE="${CONFIG_TEMPLATE:-${FLUTTER_WORKSPACE:-}/desktop-homescreen/config.toml}"
DRM_DEVICE="${1:-/dev/dri/card0}"
BUNDLE_DIR=".desktop-homescreen"

# ── FLUTTER_WORKSPACE warning (when unset) ───────────────────────────────

if [[ -z "${FLUTTER_WORKSPACE:-}" ]]; then
    cat <<'WARN' >&2

################################################################################
##                                                                            ##
##  WARNING: FLUTTER_WORKSPACE is not set.                                    ##
##                                                                            ##
##  Without it, this script can only produce a DEBUG bundle (JIT,             ##
##  kernel_blob.bin). On Pi-class hardware, debug-mode Flutter apps           ##
##  often fail to render — the Dart code runs but no frames make it           ##
##  to the panel. The kiosk service appears active but the display            ##
##  stays black (with the cursor visible on drm-kms-egl).                     ##
##                                                                            ##
##  For a RELEASE bundle (AOT, libapp.so) that renders reliably on Pi:        ##
##                                                                            ##
##      export FLUTTER_WORKSPACE=/path/to/workspace-automation                ##
##      $0 [drm_device]                                                       ##
##                                                                            ##
##  When set, this script:                                                    ##
##    • invokes $FLUTTER_WORKSPACE/flutter_workspace.py --create-aot          ##
##    • cross-compiles Dart to aarch64 via the workspace's arm64              ##
##      gen_snapshot under qemu-user against the cached PiOS sysroot          ##
##    • stages libapp.so.release into <bundle>/lib/libapp.so                  ##
##                                                                            ##
################################################################################

WARN
fi

# ── Validation ───────────────────────────────────────────────────────────

[[ -f pubspec.yaml ]] || die "run from a Flutter project root (no pubspec.yaml)"
[[ -d build/flutter_assets ]] || die "build/flutter_assets missing; run: flutter build bundle"
[[ -f "$ENGINE_BUNDLE/lib/libflutter_engine.so" ]] || die "engine not found at $ENGINE_BUNDLE/lib/libflutter_engine.so"
[[ -f "$ENGINE_BUNDLE/data/icudtl.dat" ]] || die "icudtl.dat not found at $ENGINE_BUNDLE/data/icudtl.dat"

# ── Assemble ─────────────────────────────────────────────────────────────

rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR/data" "$BUNDLE_DIR/lib"

# Engine artifacts.
cp "$ENGINE_BUNDLE/lib/libflutter_engine.so" "$BUNDLE_DIR/lib/"
cp "$ENGINE_BUNDLE/data/icudtl.dat" "$BUNDLE_DIR/data/"

# Flutter assets — copy rather than symlink so the bundle is self-contained
# and survives being moved or copied to a different directory (e.g., the
# vkms harness copies it to a tmpdir). For hot-reload workflows, use
# `flutter run -d desktop-homescreen` which manages the symlink itself.
cp -r build/flutter_assets "$BUNDLE_DIR/data/flutter_assets"

# Config — use the template if it exists, else generate a minimal one.
if [[ -f "$CONFIG_TEMPLATE" ]]; then
    cp "$CONFIG_TEMPLATE" "$BUNDLE_DIR/config.toml"
else
    cat > "$BUNDLE_DIR/config.toml" <<EOF
[global]
app_id = "homescreen"

[view]
width = 1920
height = 1080
fullscreen = true
EOF
fi

# Pin drm_device into the config.
if grep -q '^\[view\]' "$BUNDLE_DIR/config.toml"; then
    # Insert after [view]
    sed -i "/^\[view\]/a drm_device = \"$DRM_DEVICE\"" "$BUNDLE_DIR/config.toml"
else
    printf '\n[view]\ndrm_device = "%s"\n' "$DRM_DEVICE" >> "$BUNDLE_DIR/config.toml"
fi

# ── AOT cross-compile (release-mode bundle) ──────────────────────────────
#
# Only runs when FLUTTER_WORKSPACE is set. Mirrors the manual command:
#
#   QEMU_LD_PREFIX=$XC_SYSROOT \
#   GEN_SNAPSHOT=$FLUTTER_WORKSPACE/flutter/engine/src/flutter/prebuilts/linux-arm64/dart-sdk/bin/utils/gen_snapshot \
#       python3 $FLUTTER_WORKSPACE/flutter_workspace.py --create-aot --app-path $(pwd)
#
# flutter_workspace.py respects a pre-set GEN_SNAPSHOT (≥ the commit that
# introduces the "Honoring caller-set" hint) and uses it for AOT compile.
# The arm64 gen_snapshot binary runs under qemu-user via binfmt_misc,
# finding its dynamic linker via QEMU_LD_PREFIX → sysroot.

BUNDLE_RUNTIME="debug"
if [[ -n "${FLUTTER_WORKSPACE:-}" ]]; then
    GEN_SNAPSHOT="${GEN_SNAPSHOT:-$FLUTTER_WORKSPACE/flutter/engine/src/flutter/prebuilts/linux-arm64/dart-sdk/bin/utils/gen_snapshot}"
    WORKSPACE_PY="$FLUTTER_WORKSPACE/flutter_workspace.py"

    [[ -x "$GEN_SNAPSHOT" ]] \
        || die "gen_snapshot not executable: $GEN_SNAPSHOT (set GEN_SNAPSHOT to override)"
    [[ -f "$WORKSPACE_PY" ]] \
        || die "flutter_workspace.py not found at $WORKSPACE_PY"
    [[ -d "$XC_SYSROOT" ]] \
        || die "sysroot not found: $XC_SYSROOT (run scripts/build_pi.sh --prepare-only --pios $PIOS, or set XC_SYSROOT)"

    log "running flutter_workspace.py --create-aot"
    echo "    QEMU_LD_PREFIX=$XC_SYSROOT"
    echo "    GEN_SNAPSHOT=$GEN_SNAPSHOT"
    QEMU_LD_PREFIX="$XC_SYSROOT" \
    GEN_SNAPSHOT="$GEN_SNAPSHOT" \
        python3 "$WORKSPACE_PY" --create-aot --app-path "$(pwd)"

    if [[ -f "libapp.so.release" ]]; then
        log "staging libapp.so.release → $BUNDLE_DIR/lib/libapp.so"
        cp libapp.so.release "$BUNDLE_DIR/lib/libapp.so"
        # AOT bundles must NOT ship kernel_blob.bin — its presence makes
        # the embedder pick the debug code path even though libapp.so +
        # a release engine are also present.
        if [[ -f "$BUNDLE_DIR/data/flutter_assets/kernel_blob.bin" ]]; then
            rm -f "$BUNDLE_DIR/data/flutter_assets/kernel_blob.bin"
            echo "    (removed kernel_blob.bin — release bundles use AOT only)"
        fi
        BUNDLE_RUNTIME="release"
    else
        echo "WARN: libapp.so.release not produced by create_aot.py;" >&2
        echo "      bundle stays debug-mode." >&2
    fi
fi

echo
log "bundle assembled at $(pwd)/$BUNDLE_DIR"
log "  runtime   : $BUNDLE_RUNTIME"
log "  drm_device: $DRM_DEVICE"
echo
echo "Run on host with:"
echo "  $REPO_DIR/cmake-build-debug-clang/shell/homescreen -b $BUNDLE_DIR -d"
echo
echo "Provision onto a PiOS SD card:"
echo "  $SCRIPT_DIR/build_pi.sh --pios $PIOS --skip-build --device /dev/sdX \\"
echo "      --provision --bundle \$(pwd)/$BUNDLE_DIR"
