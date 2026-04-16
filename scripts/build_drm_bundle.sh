#!/usr/bin/env bash
# Copyright 2026 Toyota Connected North America
#
# Assembles a DRM/KMS-ready Flutter bundle from the current Flutter app
# directory. Run from the root of a Flutter project (where pubspec.yaml
# lives).
#
# Prerequisites:
#   - `flutter build bundle` must have been run (or `flutter run` which
#     does it implicitly). The script verifies build/flutter_assets exists.
#   - The Flutter engine artifacts (libflutter_engine.so, icudtl.dat)
#     are expected at the workspace path below. Adjust ENGINE_BUNDLE if
#     your layout differs.
#
# Output:
#   .desktop-homescreen/          (same layout the Wayland custom device
#                                  produces, with a DRM-ready config.toml)
#
# Usage:
#   cd /path/to/flutter/app
#   flutter build bundle           # if not already built
#   /path/to/ivi-homescreen/scripts/build_drm_bundle.sh [/dev/dri/cardN]
#
# The optional argument pins drm_device in config.toml. Without it, the
# backend defaults to /dev/dri/card0.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

# ── Configurable paths ───────────────────────────────────────────────────

ENGINE_BUNDLE="${ENGINE_BUNDLE:-/mnt/raid10/workspace-automation/.config/flutter_workspace/flutter-engine/bundle-debug-x64}"
CONFIG_TEMPLATE="${CONFIG_TEMPLATE:-/mnt/raid10/workspace-automation/.config/flutter_workspace/desktop-homescreen/config.toml}"
DRM_DEVICE="${1:-/dev/dri/card0}"
BUNDLE_DIR=".desktop-homescreen"

# ── Validation ───────────────────────────────────────────────────────────

die() { echo "error: $*" >&2; exit 1; }

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
EOF
fi

# Pin drm_device into the config.
if grep -q '^\[view\]' "$BUNDLE_DIR/config.toml"; then
    # Insert after [view]
    sed -i "/^\[view\]/a drm_device = \"$DRM_DEVICE\"" "$BUNDLE_DIR/config.toml"
else
    printf '\n[view]\ndrm_device = "%s"\n' "$DRM_DEVICE" >> "$BUNDLE_DIR/config.toml"
fi

echo "==> bundle assembled at $(pwd)/$BUNDLE_DIR"
echo "==> drm_device = $DRM_DEVICE"
echo ""
echo "Run with:"
echo "  $REPO_DIR/cmake-build-debug-clang/shell/homescreen -b $BUNDLE_DIR -d"
