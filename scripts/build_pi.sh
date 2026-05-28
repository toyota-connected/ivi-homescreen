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
#
#   SD card imaging + device provisioning (post-build):
#     --image-sd                    interactively detect & write the PiOS image to
#                                     an SD card. Watches lsblk before/after a prompt
#                                     for a newly-attached removable disk; refuses
#                                     non-removable disks; requires retyping the
#                                     device name to confirm.
#     --device <path>               non-interactive: image to this device (skips the
#                                     plug-in detection). Still validated for
#                                     removability + recognised path pattern.
#     --provision                   install homescreen as a systemd service into the
#                                     imaged SD card. Implied by --image-sd; pass
#                                     standalone to re-provision an already-imaged
#                                     card without rewriting the image.
#     --bundle <path>               Flutter .desktop-homescreen bundle to install
#                                     under /opt/ivi-homescreen/bundle on the target.
#                                     If omitted, only the engine SDK is staged
#                                     (useful for smoke-validating the service).
#     --service-backend <name>      pick the built backend whose binary gets
#                                     installed: wayland-egl|wayland-vulkan|drm-kms-egl
#                                     |software (default: first available, preferring
#                                     drm-kms-egl > software > wayland-egl).
#     --user <name>                 PiOS first-boot username (default: homescreen).
#                                     Without /boot/userconf.txt, PiOS Lite firstboot
#                                     refuses to come up.
#     --password <pass>             first-boot password (default: homescreen).
#     --no-mask-getty               do NOT mask getty@tty1 — keeps the login prompt
#                                     visible on tty1 (for debugging).
#     --no-deps-install             do NOT install a first-boot apt unit that
#                                     pulls the homescreen's runtime shared
#                                     libraries. Default: install the unit (so a
#                                     networked Pi self-completes); pass this for
#                                     offline kiosks where the operator pre-stages
#                                     libs themselves.
#     --skip-build                  reuse an existing build dir; just provision.
#
#     -v / --verbose
#     -h / --help
#
# Sudo is invoked only for the loopback-mount / chroot apt steps during
# sysroot preparation, and for the dd / mount / umount steps if --image-sd
# / --provision are used. The rest runs as the invoking user.

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

# SD imaging / provisioning state.
IMAGE_SD=0
PROVISION=0
TARGET_DEVICE=""
APP_BUNDLE=""
SERVICE_BACKEND=""
FIRSTBOOT_USER="homescreen"
FIRSTBOOT_PASSWORD="homescreen"
MASK_GETTY=1
INSTALL_DEPS=1
SKIP_BUILD=0

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
        --image-sd)           IMAGE_SD=1; PROVISION=1; shift ;;
        --device)             TARGET_DEVICE="$2"; IMAGE_SD=1; PROVISION=1; shift 2 ;;
        --provision)          PROVISION=1; shift ;;
        --bundle)             APP_BUNDLE="$2"; shift 2 ;;
        --service-backend)    SERVICE_BACKEND="$2"; shift 2 ;;
        --user)               FIRSTBOOT_USER="$2"; shift 2 ;;
        --password)           FIRSTBOOT_PASSWORD="$2"; shift 2 ;;
        --no-mask-getty)      MASK_GETTY=0; shift ;;
        --no-deps-install)    INSTALL_DEPS=0; shift ;;
        --skip-build)         SKIP_BUILD=1; shift ;;
        -v|--verbose)         VERBOSE=1; shift ;;
        -h|--help)            usage 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
done

case "$SERVICE_BACKEND" in
    ""|wayland-egl|wayland-vulkan|drm-kms-egl|software) ;;
    *) die "--service-backend must be wayland-egl|wayland-vulkan|drm-kms-egl|software (got: $SERVICE_BACKEND)" ;;
esac
[[ -z "$TARGET_DEVICE" || "$TARGET_DEVICE" =~ ^/dev/(sd[a-z]+|mmcblk[0-9]+|nvme[0-9]+n[0-9]+)$ ]] \
    || die "--device $TARGET_DEVICE: not a recognised path (need /dev/sd*, /dev/mmcblk*, or /dev/nvme*n*)"

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

# ── Phase 6: SD card imaging ─────────────────────────────────────────────
#
# Two-step flow: snapshot lsblk's disk list BEFORE the user is prompted,
# wait for them to attach the card, snapshot AFTER, diff to identify the
# newly-attached disk. Refuses to touch:
#   - non-removable disks  (system disk safety — /sys/block/<n>/removable)
#   - paths not matching /dev/(sd*|mmcblk*|nvme*)
#   - more than one new disk at once (operator must pick deliberately)
# Final guard before dd: operator must retype the device name.

lsblk_disks() {
    # Top-level disk-type block devices, full paths, no headers. Used by
    # the before/after diff in wait_for_sd.
    lsblk -dpno NAME,TYPE | awk '$2 == "disk" { print $1 }' | sort -u
}

wait_for_sd() {
    local before after new
    before="$(lsblk_disks)"

    echo
    echo "  Plug in the SD card now."
    echo "  (Will refuse to image internal / non-removable disks.)"
    read -r -p "  Press Enter once the card is attached… " _

    # udev typically settles within a second or two; give it some slack.
    udevadm settle --timeout=10 >/dev/null 2>&1 || true
    sleep 2

    after="$(lsblk_disks)"
    new="$(comm -13 <(echo "$before") <(echo "$after"))"

    local count
    count="$(printf '%s\n' "$new" | grep -c '^/' || true)"
    case "$count" in
        0) die "no new disk detected — check that the card mounted and retry" ;;
        1) TARGET_DEVICE="$new" ;;
        *) die "more than one new disk detected: $(echo "$new" | tr '\n' ' ')— unplug extras and retry" ;;
    esac
    log "detected SD card: $TARGET_DEVICE"
}

phase6_sd_image() {
    log "Phase 6: SD card imaging"

    if [[ -z "$TARGET_DEVICE" ]]; then
        wait_for_sd
    fi

    local name
    name="$(basename "$TARGET_DEVICE")"
    [[ -b "$TARGET_DEVICE" ]] || die "$TARGET_DEVICE is not a block device"

    # /sys/block/<name>/removable is "0" or "1". A USB SD-card reader
    # exposes the card with removable=1; a builtin mmc reader does too.
    # NVMe / SATA system disks are removable=0.
    local removable
    removable="$(cat "/sys/block/$name/removable" 2>/dev/null || echo 0)"
    [[ "$removable" == "1" ]] \
        || die "$TARGET_DEVICE is not flagged removable (got \"$removable\") — refusing to write"

    # Show identity + confirm. Retyping the device name is the kill-switch.
    echo
    echo "  Target device:"
    lsblk -dno NAME,SIZE,MODEL,VENDOR,TRAN "$TARGET_DEVICE" | sed 's/^/    /'
    echo
    echo "  This will WIPE $TARGET_DEVICE and write PiOS $PIOS to it."
    echo "  All existing data on this device will be lost."
    read -r -p "  Retype the device name (\"$name\") to confirm: " confirm
    [[ "$confirm" == "$name" ]] || die "confirmation mismatch (\"$confirm\" != \"$name\"); aborting"

    # Image file already produced by phase2_sysroot (.xz fetched + decompressed).
    local img_xz img
    img_xz="$DOWNLOADS/$(basename "$IMAGE_URL")"
    img="${img_xz%.xz}"
    [[ -s "$img" ]] || die "expected decompressed image at $img (run without --skip-build to fetch)"

    # Unmount anything currently mounted from the target. Some desktops
    # auto-mount inserted media; dd needs the partitions free.
    local part
    while read -r part; do
        [[ -n "$part" ]] || continue
        log "unmounting $part (was auto-mounted)"
        sudo umount "$part" || true
    done < <(lsblk -lnpo NAME,MOUNTPOINTS "$TARGET_DEVICE" | awk 'NF > 1 { print $1 }')

    log "writing $(basename "$img") → $TARGET_DEVICE (this is the slow part)"
    sudo dd if="$img" of="$TARGET_DEVICE" bs=4M conv=fsync status=progress
    sudo sync

    sudo partprobe "$TARGET_DEVICE" 2>/dev/null || true
    udevadm settle --timeout=10 >/dev/null 2>&1 || true

    log "image written; partitions now visible:"
    lsblk -no NAME,SIZE,TYPE,FSTYPE "$TARGET_DEVICE" | sed 's/^/    /'
}

# ── Phase 7: device provisioning ─────────────────────────────────────────
#
# Mount the imaged SD card and install:
#   /usr/local/bin/ivi-homescreen          (the binary, from build_dir_for)
#   /opt/ivi-homescreen/bundle/            (Flutter .desktop-homescreen bundle)
#   /etc/systemd/system/ivi-homescreen.service
#   /etc/systemd/system/multi-user.target.wants/ivi-homescreen.service (symlink)
#   /etc/systemd/system/getty@tty1.service → /dev/null  (mask, unless
#                                                       --no-mask-getty)
# Plus boot-partition fixups:
#   /boot/userconf.txt                     (so firstboot doesn't hang)
#   /boot/cmdline.txt                      (append quiet + nologo + nocursor)

partition_paths() {
    # Echo "boot_part root_part" for a given whole-disk device. mmcblk /
    # nvme suffix partition numbers with 'p' (e.g. mmcblk0p1); plain
    # sd[a-z] doesn't (sda1).
    local dev="$1"
    if [[ "$dev" =~ mmcblk|nvme ]]; then
        echo "${dev}p1 ${dev}p2"
    else
        echo "${dev}1 ${dev}2"
    fi
}

# Output a bcrypt-style hash (mkpasswd -m yescrypt or openssl passwd -6
# fallback). PiOS firstboot accepts /etc/shadow-format hashes.
hash_password() {
    local pass="$1"
    if command -v mkpasswd >/dev/null 2>&1; then
        echo "$pass" | mkpasswd -m yescrypt --stdin
    else
        echo "$pass" | openssl passwd -6 -stdin
    fi
}

resolve_service_backend() {
    # Pick a backend whose homescreen binary actually exists in
    # build_dir_for. Preference: drm-kms-egl > software > wayland-egl >
    # wayland-vulkan. drm-kms-egl is the "kiosk first principle" choice
    # (direct DRM, no compositor).
    local try
    if [[ -n "$SERVICE_BACKEND" ]]; then
        echo "$SERVICE_BACKEND"; return
    fi
    for try in drm-kms-egl software wayland-egl wayland-vulkan; do
        if [[ -x "$(build_dir_for "$try")/shell/homescreen" ]]; then
            echo "$try"; return
        fi
    done
    return 1
}

phase7_provision() {
    log "Phase 7: device provisioning"

    [[ -n "$TARGET_DEVICE" ]] || die "phase7 needs TARGET_DEVICE set (use --image-sd or --device)"
    [[ -b "$TARGET_DEVICE" ]] || die "$TARGET_DEVICE is not a block device"

    local p1 p2
    read -r p1 p2 <<< "$(partition_paths "$TARGET_DEVICE")"
    # udev sometimes takes another beat after partprobe.
    local i=0
    while [[ ! -b "$p1" || ! -b "$p2" ]] && (( i < 20 )); do
        sleep 0.5; udevadm settle --timeout=3 >/dev/null 2>&1 || true; i=$((i+1))
    done
    [[ -b "$p1" && -b "$p2" ]] || die "partitions not visible after partprobe: $p1, $p2"

    # Resolve a binary to install if one exists. The kiosk-service half
    # of phase 7 is best-effort — the firstboot fixups (userconf.txt,
    # ssh enable, quieted cmdline) run unconditionally so a card imaged
    # without a built binary still boots into a usable PiOS.
    local backend="" home_bin=""
    if backend="$(resolve_service_backend 2>/dev/null)"; then
        home_bin="$(build_dir_for "$backend")/shell/homescreen"
        [[ -x "$home_bin" ]] || home_bin=""
    fi
    if [[ -n "$home_bin" ]]; then
        log "will install $backend binary: $home_bin"
    else
        log "no built binary available — firstboot fixups only (no kiosk service)"
    fi

    local bundle_src=""
    if [[ -n "$APP_BUNDLE" ]]; then
        [[ -d "$APP_BUNDLE" ]] || die "bundle dir not found: $APP_BUNDLE"
        [[ -f "$APP_BUNDLE/lib/libflutter_engine.so" ]] \
            || die "bundle missing lib/libflutter_engine.so: $APP_BUNDLE"
        bundle_src="$APP_BUNDLE"
        log "bundle: $bundle_src"
    fi

    local mp_boot mp_root cleanup
    mp_boot="$(mktemp -d -t ivi-boot.XXXXXX)"
    mp_root="$(mktemp -d -t ivi-root.XXXXXX)"
    cleanup="sudo umount -lq '$mp_boot' '$mp_root' 2>/dev/null; rmdir '$mp_boot' '$mp_root' 2>/dev/null || true"
    # shellcheck disable=SC2064  # intentional: capture paths now, not at trap time
    trap "$cleanup" EXIT

    log "mounting $p1 → $mp_boot, $p2 → $mp_root"
    sudo mount "$p1" "$mp_boot"
    sudo mount "$p2" "$mp_root"

    # ── Firstboot unblock (always run) ─────────────────────────────────
    #
    # Without these, PiOS Lite's userconfig.service fails on a headless
    # boot (no interactive wizard target) and SSH is disabled by default,
    # leaving the operator with no way in. Run before any kiosk-install
    # work so a failure later doesn't leave the card stranded.

    local hash
    hash="$(hash_password "$FIRSTBOOT_PASSWORD")"
    log "writing /boot/userconf.txt (user: $FIRSTBOOT_USER)"
    echo "${FIRSTBOOT_USER}:${hash}" | sudo tee "$mp_boot/userconf.txt" >/dev/null
    sudo chmod 0600 "$mp_boot/userconf.txt"

    # Enable SSH on first boot — PiOS Lite refuses to start sshd unless
    # /boot/ssh exists. Without --no-mask-getty there's literally no
    # other way to reach the device.
    log "enabling SSH (/boot/ssh)"
    sudo touch "$mp_boot/ssh"

    # Quiet kernel cmdline so the boot is visually clean before the
    # homescreen takes over. cmdline.txt is a single line; append flags
    # idempotently.
    local cmdline="$mp_boot/cmdline.txt"
    if [[ -f "$cmdline" ]]; then
        log "quieting kernel cmdline (/boot/cmdline.txt)"
        local extra=""
        local flag
        for flag in "quiet" "logo.nologo" "vt.global_cursor_default=0" "loglevel=3"; do
            grep -qw "$flag" "$cmdline" || extra="$extra $flag"
        done
        if [[ -n "$extra" ]]; then
            sudo sed -i "s|\$|${extra}|" "$cmdline"
            note "appended:$extra"
        else
            note "cmdline already contains all flags"
        fi
    else
        note "no /boot/cmdline.txt (unexpected layout — skipping cmdline tweak)"
    fi

    # ── Kiosk install (only when a built binary is available) ─────────
    if [[ -n "$home_bin" ]]; then
        log "installing binary → /usr/local/bin/ivi-homescreen"
        sudo install -m0755 "$home_bin" "$mp_root/usr/local/bin/ivi-homescreen"

        sudo mkdir -p "$mp_root/opt/ivi-homescreen/bundle"
        if [[ -n "$bundle_src" ]]; then
            log "installing bundle → /opt/ivi-homescreen/bundle"
            sudo rsync -a --delete "$bundle_src/" "$mp_root/opt/ivi-homescreen/bundle/"
        else
            log "no --bundle given; staging engine SDK only"
            sudo mkdir -p "$mp_root/opt/ivi-homescreen/bundle/lib" \
                          "$mp_root/opt/ivi-homescreen/bundle/data"
            sudo install -m0644 "$FLUTTER_ENGINE/lib/libflutter_engine.so" \
                "$mp_root/opt/ivi-homescreen/bundle/lib/libflutter_engine.so"
            sudo install -m0644 "$FLUTTER_ENGINE/data/icudtl.dat" \
                "$mp_root/opt/ivi-homescreen/bundle/data/icudtl.dat"
        fi

        # Compose the [Unit] section. The deps service (if enabled) is a
        # hard requirement — without it the binary fails at dlopen with
        # libfoo.so.N missing.
        local unit_requires="" unit_after_deps=""
        if [[ "$INSTALL_DEPS" -eq 1 ]]; then
            unit_requires="Requires=ivi-homescreen-deps.service"
            unit_after_deps=" ivi-homescreen-deps.service"
        fi

        log "writing /etc/systemd/system/ivi-homescreen.service"
        sudo tee "$mp_root/etc/systemd/system/ivi-homescreen.service" >/dev/null <<EOF
[Unit]
Description=ivi-homescreen Flutter shell (kiosk)
DefaultDependencies=no
Wants=systemd-udev-settle.service
After=systemd-udev-settle.service local-fs.target${unit_after_deps}
${unit_requires}
# Start before getty so the homescreen owns the framebuffer / tty first.
Before=getty.target getty@tty1.service
ConditionPathExists=/opt/ivi-homescreen/bundle

[Service]
Type=simple
User=root
WorkingDirectory=/opt/ivi-homescreen
Environment=HOME=/root
Environment=XDG_RUNTIME_DIR=/run/user/0
ExecStartPre=/bin/mkdir -p /run/user/0
ExecStartPre=/bin/chmod 700 /run/user/0
ExecStart=/usr/local/bin/ivi-homescreen -b /opt/ivi-homescreen/bundle
Restart=on-failure
RestartSec=3
StandardInput=tty
StandardOutput=journal
StandardError=journal
TTYPath=/dev/tty1
TTYReset=yes
TTYVHangup=yes

[Install]
WantedBy=multi-user.target
EOF
        sudo chmod 0644 "$mp_root/etc/systemd/system/ivi-homescreen.service"

        # Enable via the multi-user.target.wants symlink (avoids a
        # `systemctl enable` chroot dance).
        sudo mkdir -p "$mp_root/etc/systemd/system/multi-user.target.wants"
        sudo ln -sf ../ivi-homescreen.service \
            "$mp_root/etc/systemd/system/multi-user.target.wants/ivi-homescreen.service"

        # Mask getty@tty1 so no login prompt paints over the homescreen.
        if [[ "$MASK_GETTY" -eq 1 ]]; then
            log "masking getty@tty1.service (no visible login prompt)"
            sudo ln -sf /dev/null "$mp_root/etc/systemd/system/getty@tty1.service"
        else
            note "--no-mask-getty: leaving getty@tty1 enabled"
        fi

        # ── Runtime shared-library installer (one-shot, first boot) ──
        #
        # PiOS Lite is minimal; the cross-build sysroot pulled in
        # `-dev` packages so the homescreen LINKED against libdrm /
        # libgbm / libdisplay-info / libgstreamer / libpipewire / …,
        # but the runtime counterparts are absent on the Lite target.
        # Install them on first network-online boot via a sentinel-
        # gated oneshot unit. ivi-homescreen.service Requires= this
        # so the kiosk doesn't fail-loop on missing .so files.
        #
        # --no-deps-install skips this for fully-offline kiosks where
        # the operator stages runtime libs themselves.
        if [[ "$INSTALL_DEPS" -eq 1 ]]; then
            # Common dependencies — touched by every backend through
            # the plugin layer or gstreamer/glib/curl deps.
            local deps_common="libxkbcommon0 libgstreamer1.0-0 \
libgstreamer-plugins-base1.0-0 libpipewire-0.3-0 libcamera0.4 \
libcurl4t64 libsecret-1-0 libjpeg62-turbo libxml2 libglib2.0-0t64"
            # Backend-specific. dmz-cursor-theme provides DMZ-White
            # (the default cursor theme drm-kms-egl loads via
            # libxcursor); without it the cursor either renders empty
            # or the homescreen falls back to no cursor.
            local deps_backend=""
            case "$backend" in
                wayland-egl)
                    deps_backend="libwayland-client0 libegl1 libgles2"
                    ;;
                wayland-vulkan)
                    deps_backend="libwayland-client0 libvulkan1 mesa-vulkan-drivers"
                    ;;
                drm-kms-egl)
                    deps_backend="libdrm2 libgbm1 libinput10 libdisplay-info2 \
libxcursor1 dmz-cursor-theme libegl1 libgles2"
                    ;;
                software)
                    deps_backend="libdrm2 libinput10"
                    ;;
            esac

            log "writing /usr/lib/ivi-homescreen/install-runtime-deps.sh"
            sudo mkdir -p "$mp_root/usr/lib/ivi-homescreen"
            sudo tee "$mp_root/usr/lib/ivi-homescreen/install-runtime-deps.sh" >/dev/null <<EOF
#!/bin/sh
# Auto-generated by build_pi.sh — installs the runtime shared
# libraries ivi-homescreen ($backend) expects to dlopen at startup.
# Runs once on first network-online boot, then the sentinel file
# /var/lib/ivi-homescreen/deps-installed disables further runs.
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \\
    $deps_common $deps_backend
EOF
            sudo chmod 0755 "$mp_root/usr/lib/ivi-homescreen/install-runtime-deps.sh"

            log "writing /etc/systemd/system/ivi-homescreen-deps.service"
            sudo tee "$mp_root/etc/systemd/system/ivi-homescreen-deps.service" >/dev/null <<EOF
[Unit]
Description=Install ivi-homescreen runtime shared libraries (first boot)
Wants=network-online.target
After=network-online.target
Before=ivi-homescreen.service
ConditionPathExists=!/var/lib/ivi-homescreen/deps-installed

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/usr/lib/ivi-homescreen/install-runtime-deps.sh
ExecStartPost=/bin/sh -c "mkdir -p /var/lib/ivi-homescreen && touch /var/lib/ivi-homescreen/deps-installed"
TimeoutStartSec=300

[Install]
WantedBy=multi-user.target
EOF
            sudo chmod 0644 "$mp_root/etc/systemd/system/ivi-homescreen-deps.service"
            sudo ln -sf ../ivi-homescreen-deps.service \
                "$mp_root/etc/systemd/system/multi-user.target.wants/ivi-homescreen-deps.service"
        else
            note "--no-deps-install: skipping runtime-deps installer unit"
        fi
    fi

    sync
    log "unmounting"
    sudo umount "$mp_boot"
    sudo umount "$mp_root"
    rmdir "$mp_boot" "$mp_root"
    trap - EXIT

    log "provisioning complete"
    echo
    if [[ -n "$home_bin" ]]; then
        echo "  Eject and boot the Pi. After firstboot the kiosk service"
        echo "  takes over the framebuffer before any visible login."
    else
        echo "  Eject and boot the Pi. PiOS firstboot will apply the"
        echo "  userconf account; the kiosk service is NOT installed"
        echo "  (no built binary was available). Re-run with a built"
        echo "  binary + --provision --skip-build to layer it on."
    fi
    echo
    echo "  Login (SSH on the .lan / .local hostname once the Pi finishes"
    echo "  firstboot, or serial console):"
    echo "    user:     $FIRSTBOOT_USER"
    echo "    password: $FIRSTBOOT_PASSWORD"
    if [[ -n "$home_bin" ]]; then
        echo
        echo "  Inspect the service on the target:"
        echo "    sudo systemctl status ivi-homescreen.service"
        echo "    sudo journalctl -u ivi-homescreen.service -b"
        if [[ "$INSTALL_DEPS" -eq 1 ]]; then
            echo
            echo "  Runtime shared libraries are installed on first network-online"
            echo "  boot by ivi-homescreen-deps.service. If that fails (no network,"
            echo "  apt mirror down), the kiosk won't start. Watch it via:"
            echo "    sudo systemctl status ivi-homescreen-deps.service"
            echo "    sudo journalctl -u ivi-homescreen-deps.service -b"
            echo "  Force a retry: rm /var/lib/ivi-homescreen/deps-installed + reboot."
        fi
    fi
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

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    for be in "${BUILD_BACKENDS[@]}"; do
        if [[ "$CLEAN" -eq 1 ]]; then
            log "wiping $(build_dir_for "$be")"
            rm -rf "$(build_dir_for "$be")"
        fi
        phase3_emit_cmake "$be"
        phase4_build "$be"
    done
else
    log "--skip-build: reusing existing build dirs"
fi

phase5_report

if [[ "$IMAGE_SD" -eq 1 ]]; then
    phase6_sd_image
fi
if [[ "$PROVISION" -eq 1 ]]; then
    phase7_provision
fi
