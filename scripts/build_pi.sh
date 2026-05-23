#!/usr/bin/env bash
# Copyright 2026 Toyota Connected North America
#
# Sandboxed aarch64 cross-build of ivi-homescreen (+ ivi-homescreen-plugins)
# for Raspberry Pi OS (Pi 4 / Pi 5 / Pi Zero 2 W). All targets share one
# aarch64 PiOS sysroot; only the -mcpu tuning differs per --target.
#
# Sandbox layout (nothing is written under /usr, /opt, etc.):
#
#   $XC_ROOT/                       default: $XDG_CACHE_HOME/ivi-homescreen-xc
#     downloads/                    cached image + toolchain + engine tarballs
#     toolchain/arm-gnu-<ver>/      extracted ARM GNU Toolchain
#     sysroot/<pios>/               PiOS rootfs + apt-installed -dev packages
#     flutter-engine/<tag>/         engine-sdk/ (lib/libflutter_engine.so, data/icudtl.dat, …)
#
#   <repo>/cmake-build-xc-pi-<pios>/   CMake build dir (already gitignored)
#
# Host requirements: x86_64 Linux only (toolchain is x86_64 → aarch64).
#
# Usage:
#   scripts/build_pi.sh [options]
#     --pios <bookworm|trixie>      PiOS release (default: bookworm)
#     --target <pi4|pi5|piz2|generic>   -mcpu tuning (default: generic)
#                                         piz2 = Pi Zero 2 W (Cortex-A53)
#     --backend <wayland-egl|wayland-vulkan|drm-kms-egl|software|all>
#                                   default: all
#                                     wayland-egl    GLES2 on Wayland
#                                     wayland-vulkan Vulkan on Wayland
#                                     drm-kms-egl    direct DRM/KMS + GBM + GLES2 (no compositor)
#                                     software       CPU rasterizer → DRM dumb buffer / fbdev
#                                     all            build every backend in its own build dir
#                                                    (drm-kms-egl is skipped on bookworm —
#                                                     libdisplay-info 0.1.1 < drm-cxx's 0.2.0)
#     --flutter-engine <path>       dir with lib/libflutter_engine.so + data/icudtl.dat
#                                     (bypasses auto-fetch)
#     --flutter-runtime <debug|debug-unopt|profile|release>   default: release
#     --flutter-engine-sha <sha>    pin a specific engine commit (default: pinned in script)
#     --engine-url <url>            override Flutter engine tarball URL
#                                   (engine is dlopen'd at runtime — not linked at build)
#     --plugins-dir <path>          default: ../ivi-homescreen-plugins/plugins
#     --no-plugins                  build homescreen only
#     --jobs <N>                    default: nproc
#     --clean                       wipe build dir before configure
#     --prepare-only                fetch/extract toolchain, sysroot, engine, then exit
#     --refresh-sysroot             rebuild sysroot from image
#     --image-url <url>             override PiOS image URL
#     --toolchain-version <ver>     ARM GNU Toolchain version (defaults: bookworm→12.3.rel1,
#                                                                       trixie→15.2.rel1)
#     --toolchain-url <url>         override toolchain tarball URL
#     -v / --verbose
#     -h / --help
#
# Sudo is invoked only for the loopback-mount / chroot apt steps during
# sysroot preparation. The rest runs as the invoking user.

set -euo pipefail

# ── Style helpers ────────────────────────────────────────────────────────

die()  { echo "error: $*" >&2; exit 1; }
log()  { echo "==> $*"; }
note() { [[ "${VERBOSE:-0}" -eq 1 ]] && echo "  · $*" || true; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

# ── Defaults ─────────────────────────────────────────────────────────────

PIOS="bookworm"
TARGET="generic"
BACKEND="all"
ALL_BACKENDS=(wayland-egl wayland-vulkan drm-kms-egl software)
FLUTTER_ENGINE=""
FLUTTER_ENGINE_EXPLICIT=0
FLUTTER_RUNTIME="release"
FLUTTER_ENGINE_SHA=""
ENGINE_URL=""
# Sibling repo layout: ivi-homescreen-plugins/plugins/CMakeLists.txt is the
# actual CMake entry; the repo root has no CMakeLists.txt.
PLUGINS_DIR="${REPO_DIR%/*}/ivi-homescreen-plugins/plugins"
NO_PLUGINS=0
JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN=0
PREPARE_ONLY=0
REFRESH_SYSROOT=0
IMAGE_URL=""
TOOLCHAIN_URL=""
VERBOSE=0

# ARM GNU Toolchain (x86_64 host → aarch64 Linux glibc).
#
# Toolchain version tracks the PiOS release. Two constraints:
#   1. Bundled glibc ≤ target glibc, else libstdc++ pulls in newer symbols
#      (e.g. __isoc23_strtoul@GLIBC_2.38) that don't resolve on-target.
#   2. libstdc++'s gthr-default.h was synced against a glibc whose
#      pthread_cond_t layout matches the target. Glibc reshuffled
#      pthread_cond_t in 2.41, so a 13.3 toolchain (built against 2.38)
#      can't compile against a 2.41 sysroot — PTHREAD_COND_INITIALIZER
#      expands to braces that no longer match the new layout.
# Pin map:
#   bookworm → 12.3.rel1 (gcc 12, glibc 2.36 — matches PiOS Bookworm)
#   trixie   → 15.2.rel1 (gcc 15, glibc ≥ 2.41 — matches PiOS Trixie)
# Override with --toolchain-version (or --toolchain-url for arbitrary URLs).
TC_TRIPLE="aarch64-none-linux-gnu"
TC_HOST="x86_64"
TC_VERSION=""        # auto-derived from $PIOS unless --toolchain-version is given
declare -A TC_VERSION_FOR_PIOS=(
    [bookworm]="12.3.rel1"
    [trixie]="15.2.rel1"
)

# Pinned PiOS images. Update these (or pass --image-url) as releases rotate.
# Trixie is the current stable arm64 channel; Bookworm has moved to oldstable.
BOOKWORM_IMG_URL="https://downloads.raspberrypi.com/raspios_oldstable_lite_arm64/images/raspios_oldstable_lite_arm64-2026-04-14/2026-04-13-raspios-bookworm-arm64-lite.img.xz"
TRIXIE_IMG_URL="https://downloads.raspberrypi.com/raspios_lite_arm64/images/raspios_lite_arm64-2026-04-21/2026-04-21-raspios-trixie-arm64-lite.img.xz"

# Pinned Flutter engine SDK from github.com/meta-flutter/flutter-engine.
# Release tags follow: linux-engine-sdk-<runtime>-<arch>-<commit-sha>
# Update ENGINE_DEFAULT_SHA to bump engine; `gh release list --repo meta-flutter/flutter-engine`
# enumerates available builds.
ENGINE_DEFAULT_SHA="13e658725ddaa270601426d1485636157e38c34c"
ENGINE_REPO="meta-flutter/flutter-engine"
ENGINE_ARCH="arm64"  # aarch64 PiOS — host is x86_64-only, no other arch supported

# ── Argument parsing ─────────────────────────────────────────────────────

usage() {
    awk 'NR==1{next} /^[^#]/{exit} {sub(/^# ?/,""); print}' "$0"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pios)               PIOS="$2"; shift 2 ;;
        --target)             TARGET="$2"; shift 2 ;;
        --backend)            BACKEND="$2"; shift 2 ;;
        --flutter-engine)     FLUTTER_ENGINE="$2"; FLUTTER_ENGINE_EXPLICIT=1; shift 2 ;;
        --flutter-runtime)    FLUTTER_RUNTIME="$2"; shift 2 ;;
        --flutter-engine-sha) FLUTTER_ENGINE_SHA="$2"; shift 2 ;;
        --engine-url)         ENGINE_URL="$2"; shift 2 ;;
        --plugins-dir)        PLUGINS_DIR="$2"; shift 2 ;;
        --no-plugins)         NO_PLUGINS=1; shift ;;
        --jobs)               JOBS="$2"; shift 2 ;;
        --clean)              CLEAN=1; shift ;;
        --prepare-only)       PREPARE_ONLY=1; shift ;;
        --refresh-sysroot)    REFRESH_SYSROOT=1; shift ;;
        --image-url)          IMAGE_URL="$2"; shift 2 ;;
        --toolchain-url)      TOOLCHAIN_URL="$2"; shift 2 ;;
        --toolchain-version)  TC_VERSION="$2"; shift 2 ;;
        -v|--verbose)         VERBOSE=1; shift ;;
        -h|--help)            usage 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

case "$PIOS" in bookworm|trixie) ;; *) die "--pios must be bookworm|trixie (got: $PIOS)" ;; esac
case "$TARGET" in pi4|pi5|piz2|generic) ;; *) die "--target must be pi4|pi5|piz2|generic (got: $TARGET)" ;; esac
case "$BACKEND" in wayland-egl|wayland-vulkan|drm-kms-egl|software|all) ;;
    *) die "--backend must be wayland-egl|wayland-vulkan|drm-kms-egl|software|all (got: $BACKEND)" ;;
esac

# Resolve which backends to actually build. drm-kms-egl is unavailable on
# bookworm because drm-cxx requires libdisplay-info >= 0.2.0 and bookworm
# only ships 0.1.1.
if [[ "$BACKEND" == "all" ]]; then
    BUILD_BACKENDS=("${ALL_BACKENDS[@]}")
    if [[ "$PIOS" == "bookworm" ]]; then
        BUILD_BACKENDS=()
        for be in "${ALL_BACKENDS[@]}"; do
            [[ "$be" == "drm-kms-egl" ]] && continue
            BUILD_BACKENDS+=("$be")
        done
    fi
else
    BUILD_BACKENDS=("$BACKEND")
fi
case "$FLUTTER_RUNTIME" in debug|debug-unopt|profile|release) ;;
    *) die "--flutter-runtime must be debug|debug-unopt|profile|release (got: $FLUTTER_RUNTIME)" ;;
esac

[[ -z "$FLUTTER_ENGINE_SHA" ]] && FLUTTER_ENGINE_SHA="$ENGINE_DEFAULT_SHA"
ENGINE_TAG="linux-engine-sdk-${FLUTTER_RUNTIME}-${ENGINE_ARCH}-${FLUTTER_ENGINE_SHA}"
[[ -z "$ENGINE_URL" ]] && \
    ENGINE_URL="https://github.com/${ENGINE_REPO}/releases/download/${ENGINE_TAG}/${ENGINE_TAG}.tar.gz"

case "$TARGET" in
    pi4)     XC_CPU_FLAGS="-mcpu=cortex-a72" ;;
    pi5)     XC_CPU_FLAGS="-mcpu=cortex-a76" ;;
    piz2)    XC_CPU_FLAGS="-mcpu=cortex-a53" ;;
    generic) XC_CPU_FLAGS="" ;;  # ARMv8-A baseline; runs on Pi 4 / Pi 5 / Pi Zero 2 W
esac

if [[ -z "$IMAGE_URL" ]]; then
    case "$PIOS" in
        bookworm) IMAGE_URL="$BOOKWORM_IMG_URL" ;;
        trixie)
            [[ -n "$TRIXIE_IMG_URL" ]] \
                || die "PiOS Trixie has no pinned image URL yet; pass --image-url <url>"
            IMAGE_URL="$TRIXIE_IMG_URL" ;;
    esac
fi

# Toolchain version: explicit > per-PiOS default. Then derive the tarball
# name / URL / extracted-directory name.
[[ -z "$TC_VERSION" ]] && TC_VERSION="${TC_VERSION_FOR_PIOS[$PIOS]:-}"
[[ -n "$TC_VERSION" ]] \
    || die "no toolchain version mapped for --pios $PIOS; pass --toolchain-version <ver>"
TC_TARBALL="arm-gnu-toolchain-${TC_VERSION}-${TC_HOST}-${TC_TRIPLE}.tar.xz"
TC_DIRNAME="arm-gnu-toolchain-${TC_VERSION}-${TC_HOST}-${TC_TRIPLE}"
[[ -z "$TOOLCHAIN_URL" ]] \
    && TOOLCHAIN_URL="https://developer.arm.com/-/media/Files/downloads/gnu/${TC_VERSION}/binrel/${TC_TARBALL}"

# ── Resolved sandbox paths ───────────────────────────────────────────────

XC_ROOT="${IVI_XC_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/ivi-homescreen-xc}"
DOWNLOADS="$XC_ROOT/downloads"
TC_DIR="$XC_ROOT/toolchain/$TC_DIRNAME"
CROSS_BIN="$TC_DIR/bin"
XC_SYSROOT="$XC_ROOT/sysroot/$PIOS"
ENGINE_CACHE_DIR="$XC_ROOT/flutter-engine/$ENGINE_TAG"
ENGINE_SDK_DIR="$ENGINE_CACHE_DIR/engine-sdk"
# BUILD_DIR is per-backend; emitted by build_dir_for() below.

build_dir_for() { echo "$REPO_DIR/cmake-build-xc-pi-${PIOS}-$1"; }

export XC_ROOT CROSS_BIN XC_SYSROOT XC_CPU_FLAGS

# ── Phase 0: preflight ───────────────────────────────────────────────────

phase0_preflight() {
    log "Phase 0: preflight"
    local missing=()
    for tool in curl xz tar sfdisk rsync sudo cmake pkg-config sha256sum file; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done
    command -v qemu-aarch64-static >/dev/null 2>&1 \
        || command -v /usr/bin/qemu-aarch64-static >/dev/null 2>&1 \
        || missing+=("qemu-aarch64-static (package: qemu-user-static)")

    if (( ${#missing[@]} )); then
        echo "missing host tools:" >&2
        printf '  - %s\n' "${missing[@]}" >&2
        echo
        echo "  Debian/Ubuntu: sudo apt install curl xz-utils tar fdisk rsync cmake pkg-config qemu-user-static" >&2
        echo "  Fedora:        sudo dnf install curl xz tar util-linux rsync cmake pkgconf-pkg-config qemu-user-static" >&2
        exit 1
    fi

    mkdir -p "$DOWNLOADS" "$XC_ROOT/toolchain" "$XC_ROOT/sysroot" "$XC_ROOT/flutter-engine"
    note "XC_ROOT      = $XC_ROOT"
    note "Backends     = ${BUILD_BACKENDS[*]}"
    for be in "${BUILD_BACKENDS[@]}"; do
        note "  $be → $(build_dir_for "$be")"
    done
}

# ── Phase 1: toolchain ───────────────────────────────────────────────────

fetch() {
    # fetch <url> <dest>   resumable curl
    local url="$1" dest="$2"
    if [[ -s "$dest" ]]; then
        note "cached: $(basename "$dest")"; return
    fi
    log "fetching $(basename "$dest")"
    curl --fail --location --retry 3 --retry-delay 2 \
         --continue-at - --output "$dest.part" "$url"
    mv "$dest.part" "$dest"
}

verify_sha256() {
    # verify_sha256 <file> <sha256-hex>
    local file="$1" want="$2" got
    got="$(sha256sum "$file" | awk '{print $1}')"
    [[ "$got" == "$want" ]] || die "sha256 mismatch for $file (got $got, want $want)"
    note "sha256 ok: $(basename "$file")"
}

verify_sha256_file() {
    # verify_sha256_file <file> <sha256-url>   (companion .sha256 next to artifact)
    local file="$1" url="$2"
    local sha_local="$DOWNLOADS/$(basename "$file").sha256"
    if curl --fail --silent --location --output "$sha_local" "$url" 2>/dev/null; then
        (cd "$(dirname "$file")" && sha256sum -c "$sha_local") >/dev/null \
            || die "sha256 mismatch for $file"
        note "sha256 ok: $(basename "$file")"
    else
        echo "  WARN: no .sha256 alongside $url — skipping integrity check" >&2
    fi
}

phase1_toolchain() {
    log "Phase 1: toolchain ($TC_DIRNAME)"
    if [[ -x "$CROSS_BIN/${TC_TRIPLE}-gcc" ]]; then
        note "toolchain present"
        return
    fi
    local tarball="$DOWNLOADS/$TC_TARBALL"
    fetch "$TOOLCHAIN_URL" "$tarball"
    # ARM publishes .sha256.asc next to the tarball; use the plain .sha256 mirror if available.
    verify_sha256_file "$tarball" "${TOOLCHAIN_URL}.sha256"
    log "extracting toolchain"
    tar -xJf "$tarball" -C "$XC_ROOT/toolchain"
    [[ -x "$CROSS_BIN/${TC_TRIPLE}-gcc" ]] \
        || die "toolchain extracted but $CROSS_BIN/${TC_TRIPLE}-gcc not found"
}

# ── Phase 1b: Flutter engine SDK ─────────────────────────────────────────
#
# Note: ivi-homescreen does NOT link against libflutter_engine.so. The shell
# dlopens it at runtime (shell/libflutter_engine.cc). The fetched SDK is a
# *runtime* artifact — it supplies libflutter_engine.so + icudtl.dat to be
# co-located with the homescreen binary at deployment time.

phase1b_flutter_engine() {
    # If --flutter-engine was given, the user is supplying their own bundle.
    # Trust it; just verify the required artifacts exist.
    if [[ "$FLUTTER_ENGINE_EXPLICIT" -eq 1 ]]; then
        log "Phase 1b: Flutter engine (user-supplied)"
        [[ -f "$FLUTTER_ENGINE/lib/libflutter_engine.so" ]] \
            || die "missing $FLUTTER_ENGINE/lib/libflutter_engine.so"
        [[ -f "$FLUTTER_ENGINE/data/icudtl.dat" ]] \
            || die "missing $FLUTTER_ENGINE/data/icudtl.dat"
        return
    fi

    log "Phase 1b: Flutter engine SDK ($ENGINE_TAG)"
    if [[ -f "$ENGINE_SDK_DIR/lib/libflutter_engine.so" \
       && -f "$ENGINE_SDK_DIR/data/icudtl.dat" ]]; then
        note "engine SDK present"
        FLUTTER_ENGINE="$ENGINE_SDK_DIR"
        return
    fi

    local tarball="$DOWNLOADS/${ENGINE_TAG}.tar.gz"
    fetch "$ENGINE_URL" "$tarball"
    verify_sha256_file "$tarball" "${ENGINE_URL}.sha256"

    log "extracting engine SDK → $ENGINE_CACHE_DIR"
    mkdir -p "$ENGINE_CACHE_DIR"
    # Tarball layout: flutter/engine/src/out/linux_<runtime>_<arch>/engine-sdk/...
    # Strip 5 levels so engine-sdk/ lands directly under $ENGINE_CACHE_DIR.
    tar -xzf "$tarball" -C "$ENGINE_CACHE_DIR" --strip-components=5
    [[ -f "$ENGINE_SDK_DIR/lib/libflutter_engine.so" ]] \
        || die "engine extracted but $ENGINE_SDK_DIR/lib/libflutter_engine.so missing"
    [[ -f "$ENGINE_SDK_DIR/data/icudtl.dat" ]] \
        || die "engine extracted but $ENGINE_SDK_DIR/data/icudtl.dat missing"
    FLUTTER_ENGINE="$ENGINE_SDK_DIR"
}

# ── Phase 2: sysroot ─────────────────────────────────────────────────────

# Relativize absolute symlinks under $1 so the moved sysroot resolves correctly.
relativize_symlinks() {
    local root="$1" link target rel
    while IFS= read -r link; do
        target="$(readlink "$link")"
        # python is widely available; relpath is fiddly in pure shell.
        rel="$(python3 -c "import os,sys; print(os.path.relpath(sys.argv[1], os.path.dirname(sys.argv[2])))" \
                "${root}${target}" "$link")"
        ln -snf "$rel" "$link"
    done < <(find "$root" -type l -lname '/*')
}

phase2_sysroot() {
    log "Phase 2: sysroot ($PIOS)"
    if [[ -d "$XC_SYSROOT" && "$REFRESH_SYSROOT" -eq 0 ]]; then
        note "sysroot present"
        return
    fi
    if [[ "$REFRESH_SYSROOT" -eq 1 ]]; then
        log "removing existing sysroot for refresh"
        sudo rm -rf "$XC_SYSROOT"
    fi

    local img_xz img
    img_xz="$DOWNLOADS/$(basename "$IMAGE_URL")"
    img="${img_xz%.xz}"

    fetch "$IMAGE_URL" "$img_xz"
    verify_sha256_file "$img_xz" "${IMAGE_URL}.sha256"

    if [[ ! -s "$img" ]]; then
        log "decompressing $(basename "$img_xz")"
        xz -dkT0 "$img_xz"
    fi

    # Find rootfs partition (Linux native, type 83).
    log "locating rootfs partition"
    local part_start part_offset
    part_start="$(sfdisk -J "$img" \
        | python3 -c '
import json, sys
parts = json.load(sys.stdin)["partitiontable"]["partitions"]
linux = [p for p in parts if p.get("type") in ("83", "linux")]
if not linux: sys.exit("no Linux partition")
print(linux[0]["start"])')"
    part_offset=$(( part_start * 512 ))
    note "rootfs offset: $part_offset bytes"

    mkdir -p "$XC_SYSROOT"
    local mnt
    mnt="$(mktemp -d)"
    log "mounting rootfs (sudo required)"
    local loop
    loop="$(sudo losetup --show -f -P --offset "$part_offset" "$img")"
    trap "sudo umount -q '$mnt' || true; sudo losetup -d '$loop' || true; rmdir '$mnt' || true" EXIT
    sudo mount -o ro "$loop" "$mnt"

    log "rsyncing rootfs → $XC_SYSROOT"
    # No -A/-X: source ext4 carries security.selinux xattrs that the host FS
    # may reject (rsync exits 23). They aren't needed in a cross sysroot.
    sudo rsync -aH --numeric-ids \
        --exclude=/proc/* --exclude=/sys/* --exclude=/dev/* --exclude=/run/* \
        --exclude=/tmp/*  --exclude=/var/cache/apt/archives/* \
        "$mnt/" "$XC_SYSROOT/"

    sudo umount "$mnt"; sudo losetup -d "$loop"; rmdir "$mnt"
    trap - EXIT

    sudo chown -R "$(id -u):$(id -g)" "$XC_SYSROOT" 2>/dev/null || true

    log "relativizing absolute symlinks"
    relativize_symlinks "$XC_SYSROOT"

    log "installing -dev packages via qemu-aarch64-static chroot"
    local qemu_bin
    qemu_bin="$(command -v qemu-aarch64-static || echo /usr/bin/qemu-aarch64-static)"
    sudo cp "$qemu_bin" "$XC_SYSROOT/usr/bin/qemu-aarch64-static"

    # apt/gpgv/dpkg need /dev/null, /proc, /sys inside the chroot.
    sudo mount --bind /dev      "$XC_SYSROOT/dev"
    sudo mount --bind /dev/pts  "$XC_SYSROOT/dev/pts"
    sudo mount -t proc  proc    "$XC_SYSROOT/proc"
    sudo mount -t sysfs sysfs   "$XC_SYSROOT/sys"
    trap "sudo umount -lq '$XC_SYSROOT/sys' '$XC_SYSROOT/proc' '$XC_SYSROOT/dev/pts' '$XC_SYSROOT/dev' 2>/dev/null || true; sudo rm -f '$XC_SYSROOT/usr/bin/qemu-aarch64-static'" EXIT

    # Stub out post-install hooks that assume a real running system. We never
    # boot this sysroot — update-initramfs would try to mkinitramfs against
    # the host's /, and policy-rc.d=101 blocks service start in maintainer scripts.
    printf '#!/bin/sh\nexit 0\n' | sudo tee "$XC_SYSROOT/usr/sbin/update-initramfs" >/dev/null
    sudo chmod +x "$XC_SYSROOT/usr/sbin/update-initramfs"
    printf '#!/bin/sh\nexit 101\n' | sudo tee "$XC_SYSROOT/usr/sbin/policy-rc.d" >/dev/null
    sudo chmod +x "$XC_SYSROOT/usr/sbin/policy-rc.d"

    # Shared deps across all backends (plugins, GLES, gstreamer/glib, etc.).
    # libsystemd-dev is for sdbus-cpp inside ivi-homescreen-plugins.
    local pkgs=(
        libcamera-dev libcurl4-openssl-dev libegl-dev libgles2-mesa-dev
        libglib2.0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
        libjpeg-dev libpipewire-0.3-dev libsecret-1-dev libsystemd-dev
        libudev-dev libxkbcommon-dev libxml2-dev zlib1g-dev
    )
    # Backend-specific deps — union of every backend queued in BUILD_BACKENDS.
    # Duplicate package names are harmless: apt resolves them once.
    for be in "${BUILD_BACKENDS[@]}"; do
        case "$be" in
            wayland-egl)
                pkgs+=(libwayland-dev wayland-protocols) ;;
            wayland-vulkan)
                pkgs+=(libwayland-dev wayland-protocols libvulkan-dev mesa-vulkan-drivers) ;;
            drm-kms-egl)
                # drm-cxx requires libdisplay-info >= 0.2.0 (Trixie 0.2.0;
                # Bookworm 0.1.1 — drm-kms-egl is pre-filtered out for bookworm).
                # libxcursor-dev gates the DRM HW cursor module; absent → leaky
                # symbol references (~DrmCursor / DrmCursor::Move) break the link.
                pkgs+=(libdrm-dev libgbm-dev libinput-dev libdisplay-info-dev
                       libxcursor-dev) ;;
            software)
                pkgs+=(libdrm-dev libinput-dev) ;;
        esac
    done

    sudo chroot "$XC_SYSROOT" /usr/bin/qemu-aarch64-static /usr/bin/apt-get -y update
    sudo chroot "$XC_SYSROOT" /usr/bin/qemu-aarch64-static /usr/bin/apt-get -y \
        install --no-install-recommends "${pkgs[@]}"
    sudo chroot "$XC_SYSROOT" /usr/bin/qemu-aarch64-static /usr/bin/apt-get -y clean

    sudo umount -lq "$XC_SYSROOT/sys" "$XC_SYSROOT/proc" "$XC_SYSROOT/dev/pts" "$XC_SYSROOT/dev"
    trap - EXIT
    sudo rm -f "$XC_SYSROOT/usr/bin/qemu-aarch64-static"

    # apt-in-chroot created root-owned dirs. Hand them back to the user so the
    # rest of the script (and the build dir's compiler/find) can read them.
    sudo chown -R "$(id -u):$(id -g)" "$XC_SYSROOT"

    log "re-relativizing symlinks after apt"
    relativize_symlinks "$XC_SYSROOT"

    # glibc 2.34+ merged librt / libdl / libpthread into libc; Trixie's
    # libc6-dev no longer ships the unversioned dev symlinks (libfoo.so).
    # find_library(rt|dl|pthread) in CMake fails as a result. Recreate the
    # symlinks pointing at the empty-stub libfoo.so.<N> the runtime still ships.
    log "fixing up missing libc-merged dev symlinks"
    local ma="$XC_SYSROOT/usr/lib/aarch64-linux-gnu"
    for stem in rt dl pthread; do
        if [[ ! -e "$ma/lib${stem}.so" ]]; then
            local sover
            sover="$(ls "$ma" | grep -E "^lib${stem}\.so\.[0-9]+$" | head -1)"
            [[ -n "$sover" ]] || { note "no lib${stem}.so.* present, skipping"; continue; }
            ln -sf "$sover" "$ma/lib${stem}.so"
            note "created $ma/lib${stem}.so -> $sover"
        fi
    done
}

# ── Phase 3: toolchain file + pkg-config wrapper ─────────────────────────

phase3_emit_cmake() {
    # phase3_emit_cmake <backend>
    local be="$1" BUILD_DIR
    BUILD_DIR="$(build_dir_for "$be")"
    log "Phase 3: emit toolchain file & pkg-config wrapper ($be)"
    mkdir -p "$BUILD_DIR"

    cat > "$BUILD_DIR/.xc-pkg-config" <<'EOF'
#!/bin/sh
# Generated by build_pi.sh — sysroot-aware pkg-config wrapper.
export PKG_CONFIG_DIR=
export PKG_CONFIG_LIBDIR="$XC_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$XC_SYSROOT/usr/lib/pkgconfig:$XC_SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$XC_SYSROOT"
exec pkg-config "$@"
EOF
    chmod +x "$BUILD_DIR/.xc-pkg-config"

    cat > "$BUILD_DIR/.xc-toolchain.cmake" <<EOF
# Generated by scripts/build_pi.sh — do not edit.
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT          "\$ENV{XC_SYSROOT}")

# Debian's multiarch dirs (usr/lib/aarch64-linux-gnu) aren't on CMake's
# default find_library search path; gcc -print-multiarch is empty for this
# bare-Linux triplet, so CMake can't auto-detect it. Set it explicitly so
# find_library(rt|dl|...) finds the libs there.
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_C_COMPILER       "\$ENV{CROSS_BIN}/${TC_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER     "\$ENV{CROSS_BIN}/${TC_TRIPLE}-g++")
set(CMAKE_FIND_ROOT_PATH   "\$ENV{XC_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ARM GNU Toolchain targets the aarch64-none-linux-gnu triplet, but Debian
# splits files between two locations based on the aarch64-linux-gnu (no
# -none-) multiarch triplet:
#   headers: /usr/include/aarch64-linux-gnu/    (bits/wordsize.h, etc.)
#   libs:    /usr/lib/aarch64-linux-gnu/        (crt1.o, libm.so, etc.)
#            /lib/aarch64-linux-gnu/            (libc runtime)
# Native Debian GCC bakes these into its default search paths; the ARM GNU
# Toolchain doesn't. Splice them in explicitly.
set(_xc_ma_inc "\${CMAKE_SYSROOT}/usr/include/aarch64-linux-gnu")
set(_xc_ma_usr "\${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu")
set(_xc_ma_lib "\${CMAKE_SYSROOT}/lib/aarch64-linux-gnu")
set(CMAKE_C_FLAGS_INIT     "-isystem \${_xc_ma_inc} -B\${_xc_ma_usr} \$ENV{XC_CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT   "-isystem \${_xc_ma_inc} -B\${_xc_ma_usr} \$ENV{XC_CPU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-L\${_xc_ma_usr} -L\${_xc_ma_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L\${_xc_ma_usr} -L\${_xc_ma_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-L\${_xc_ma_usr} -L\${_xc_ma_lib}")

# CMake's compiler-ABI test (CMakeTestCXXCompiler) ignores
# CMAKE_*_LINKER_FLAGS_INIT, so it tries to link cmTC_xxx without the -L
# paths above and fails to find crt1.o / -lm. Telling try_compile() to
# build a static library skips the link step entirely, while the actual
# project build still uses the flags above.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# CACHE FORCE so the value survives find_program() in nested CMakeLists
# (e.g. plugins/.../sdbus-cpp does its own find_package(PkgConfig REQUIRED)).
set(PKG_CONFIG_EXECUTABLE  "$BUILD_DIR/.xc-pkg-config"
    CACHE FILEPATH "sysroot-aware pkg-config wrapper" FORCE)
EOF
    note "toolchain file: $BUILD_DIR/.xc-toolchain.cmake"
}

# ── Phase 4: configure & build ───────────────────────────────────────────

phase4_build() {
    # phase4_build <backend>
    local be="$1" BUILD_DIR
    BUILD_DIR="$(build_dir_for "$be")"
    log "Phase 4: configure & build ($be)"

    local cmake_args=(
        -S "$REPO_DIR" -B "$BUILD_DIR"
        -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/.xc-toolchain.cmake"
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_BACKEND_HEADLESS_EGL=OFF
    )
    # Exactly one backend is enabled; the rest are forced OFF.
    case "$be" in
        wayland-egl)
            cmake_args+=(
                -DBUILD_BACKEND_WAYLAND_EGL=ON
                -DBUILD_BACKEND_WAYLAND_VULKAN=OFF
                -DBUILD_BACKEND_DRM_KMS_EGL=OFF
                -DBUILD_BACKEND_SOFTWARE=OFF) ;;
        wayland-vulkan)
            cmake_args+=(
                -DBUILD_BACKEND_WAYLAND_EGL=OFF
                -DBUILD_BACKEND_WAYLAND_VULKAN=ON
                -DBUILD_BACKEND_DRM_KMS_EGL=OFF
                -DBUILD_BACKEND_SOFTWARE=OFF) ;;
        drm-kms-egl)
            cmake_args+=(
                -DBUILD_BACKEND_WAYLAND_EGL=OFF
                -DBUILD_BACKEND_WAYLAND_VULKAN=OFF
                -DBUILD_BACKEND_DRM_KMS_EGL=ON
                -DBUILD_BACKEND_SOFTWARE=OFF) ;;
        software)
            cmake_args+=(
                -DBUILD_BACKEND_WAYLAND_EGL=OFF
                -DBUILD_BACKEND_WAYLAND_VULKAN=OFF
                -DBUILD_BACKEND_DRM_KMS_EGL=OFF
                -DBUILD_BACKEND_SOFTWARE=ON) ;;
    esac

    if [[ "$NO_PLUGINS" -eq 1 ]]; then
        cmake_args+=(-DDISABLE_PLUGINS=ON)
    else
        [[ -d "$PLUGINS_DIR" ]] || die "plugins dir not found: $PLUGINS_DIR (use --no-plugins or --plugins-dir)"
        cmake_args+=(-DDISABLE_PLUGINS=OFF -DPLUGINS_DIR="$PLUGINS_DIR")
    fi

    # No engine flag is passed to CMake: libflutter_engine.so is dlopen'd at
    # runtime, not linked. The fetched SDK is staged for bundle assembly.

    log "cmake configure"
    cmake "${cmake_args[@]}"

    log "cmake build (jobs=$JOBS)"
    cmake --build "$BUILD_DIR" -j "$JOBS"
}

# ── Phase 5: report ──────────────────────────────────────────────────────

phase5_report() {
    log "Phase 5: artifacts"
    local be BUILD_DIR exe
    for be in "${BUILD_BACKENDS[@]}"; do
        BUILD_DIR="$(build_dir_for "$be")"
        exe="$BUILD_DIR/shell/homescreen"
        echo "  [$be]"
        if [[ -x "$exe" ]]; then
            echo "    binary : $exe"
            file "$exe" | sed 's/^/      /'
        else
            echo "    (no homescreen binary produced)"
        fi
    done
    echo "  sysroot       : $XC_SYSROOT"
    echo "  toolchain     : $TC_DIR"
    echo "  engine (runtime): $FLUTTER_ENGINE"
    echo
    echo "Rebuild a single backend:"
    echo "  cmake --build $(build_dir_for "${BUILD_BACKENDS[0]}") -j $JOBS"
    echo "Bundle (per backend):"
    echo "  FLUTTER_WORKSPACE=... ENGINE_BUNDLE=$FLUTTER_ENGINE \\"
    echo "    scripts/build_drm_bundle.sh    (from your Flutter app dir)"
}

# ── Main ─────────────────────────────────────────────────────────────────

phase0_preflight
phase1_toolchain
phase1b_flutter_engine
phase2_sysroot

if [[ "$PREPARE_ONLY" -eq 1 ]]; then
    log "prepare-only: stopping before configure"
    echo "  toolchain: $TC_DIR"
    echo "  sysroot  : $XC_SYSROOT"
    echo "  engine   : $FLUTTER_ENGINE"
    exit 0
fi

if [[ "$BACKEND" == "all" && "$PIOS" == "bookworm" ]]; then
    log "note: skipping drm-kms-egl on bookworm (libdisplay-info 0.1.1 < drm-cxx 0.2.0)"
fi

for be in "${BUILD_BACKENDS[@]}"; do
    if [[ "$CLEAN" -eq 1 ]]; then
        log "wiping $(build_dir_for "$be")"
        rm -rf "$(build_dir_for "$be")"
    fi
    phase3_emit_cmake "$be"
    phase4_build "$be"
done

phase5_report
