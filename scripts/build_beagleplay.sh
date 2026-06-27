#!/usr/bin/env bash
# Copyright 2026 Toyota Connected North America
#
# Sandboxed aarch64 cross-build of ivi-homescreen (+ ivi-homescreen-plugins)
# for the BeagleBoard.org BeaglePlay (TI Sitara AM625, 4x Cortex-A53), modeled
# on scripts/build_pi.sh: one aarch64 Debian sysroot extracted from the official
# image, an ARM GNU Toolchain, and a per-backend CMake build dir.
#
# GPU / Vulkan stack
# ------------------
# The AM625 integrates an Imagination PowerVR Rogue AXE-1-16M GPU. As of 2026 it
# is supported by the FULLY UPSTREAM open-source stack — the `powervr` DRM kernel
# driver (render node) plus the Mesa `pvr` Vulkan driver — giving Vulkan 1.2 with
# no proprietary blobs or out-of-tree patches. Display is the TI DSS (`tidss` DRM
# driver). BeagleBoard ships two kernel flavors of the same Debian release:
#   * v6.18.x-k3  — mainline 6.18 LTS; carries the upstream PowerVR + Vulkan stack
#   * v6.12.x-ti  — older TI BSP kernel; GLES-focused, not the Vulkan target
# This script defaults to the v6.18-k3 image so the device has the latest kernel
# and GPU stack that supports Vulkan. (Vulkan 1.2 was demonstrated on Linux 6.18
# + Mesa 25.3; the Debian image ships a Mesa in that series. Confirm on-target
# with `vulkaninfo`.)
#
# Note: ivi-homescreen does NOT link libvulkan — vulkan.hpp dlopens it at runtime
# and the Vulkan headers are vendored — so the cross sysroot only needs the DRM /
# GBM / seat / input -dev packages. The Vulkan loader + the PowerVR Mesa ICD +
# the powervr GPU firmware are RUNTIME requirements that ship in the image above.
#
# Sandbox layout (nothing is written under /usr, /opt, etc.):
#
#   $XC_ROOT/                       default: $XDG_CACHE_HOME/ivi-homescreen-xc-beagleplay
#     downloads/                    cached image + toolchain + engine tarballs
#     toolchain/arm-gnu-<ver>/      extracted ARM GNU Toolchain
#     sysroot/beagleplay/           Debian rootfs + apt-installed -dev packages
#     flutter-engine/<tag>/         engine-sdk/ (lib/libflutter_engine.so, data/icudtl.dat)
#
#   <repo>/cmake-build-xc-beagleplay-<backend>/   CMake build dir (gitignored)
#
# Host requirements: x86_64 (or aarch64) Linux + qemu-user-static. The sysroot
# is built WITHOUT sudo when the host has fuse2fs and unprivileged user
# namespaces (rootless ext4 mount + fake-root qemu chroot); otherwise it falls
# back to a sudo loop-mount + chroot (as build_pi.sh does). Force the latter
# with --sudo-sysroot.
#
# Usage:
#   scripts/build_beagleplay.sh [options]
#     --backend <wayland-egl|wayland-vulkan|drm-kms-egl|drm-kms-vulkan|software|all>
#                                   default: drm-kms-vulkan
#     --kernel <k3|ti>             image kernel flavor (default: k3 = v6.18,
#                                   the upstream PowerVR + Vulkan stack)
#     --flutter-engine <path>       dir with lib/libflutter_engine.so + data/icudtl.dat
#     --flutter-runtime <debug|debug-unopt|profile|release>   default: release
#     --flutter-engine-sha <sha>    pin a specific engine commit
#     --engine-url <url>            override the engine tarball URL
#     --plugins-dir <path>          default: ../ivi-homescreen-plugins/plugins
#     --no-plugins                  build homescreen only
#     --with-vk-probe               also build drm_kms_vulkan_probe (standalone
#                                   zero-copy dma-buf scanout capability probe;
#                                   run on-target to check the PowerVR Vulkan
#                                   driver exposes the required extensions)
#     --with-scene                  drm-kms-egl only: plane compositor + LayerScene
#     --jobs <N>                    default: nproc
#     --clean                       wipe build dir before configure
#     --prepare-only                fetch/extract toolchain, sysroot, engine, exit
#     --refresh-sysroot             rebuild the sysroot from the image
#     --sudo-sysroot                force the sudo loop-mount + chroot path
#     --image-url <url>             override the Debian image URL
#     --image-sha256 <hex>          override the image checksum
#     --toolchain-version <ver>     ARM GNU Toolchain version (default 15.2.rel1)
#     --toolchain-url <url>         override toolchain tarball URL
#     --toolchain-host <arch>       toolchain build-host arch (auto: x86_64/aarch64)
#
#   Deploy (post-build, optional):
#     --image-sd <dev>              flash the Debian image to a card (e.g.
#                                     /dev/sda), verifying its SHA256 first. ERASES
#                                     the device (removable-only). If --bundle is
#                                     also given, stages onto it after flashing —
#                                     so flash + stage is one command.
#     --stage-sd <dev>              offline-stage binary + engine + bundle onto a
#                                     flashed card (e.g. /dev/sda) under
#                                     /opt/ivi-homescreen. Needs sudo (mounts the
#                                     card's ext4 rootfs). Pair with --bundle.
#     --bundle <dir>                Flutter bundle dir (lib/libflutter_engine.so +
#                                     data/{icudtl.dat,flutter_assets}) to stage.
#     --provision                   with --stage-sd: install + enable the systemd
#                                     kiosk unit and mask the display-manager.
#     --no-verify-flash             skip the post-flash readback compare (faster,
#                                     but won't catch a card that drops mid-write).
#     --deploy <user@host>          scp the built binary to the BeaglePlay and
#                                   print the run command. The Flutter engine +
#                                   a bundle are deployed separately by the
#                                   operator (engine is dlopen'd at runtime).
#     --drm-device <node>           DRM scanout node to suggest in the run hint
#                                   (default: /dev/dri/card0 — tidss display)
#
#     -v / --verbose
#     -h / --help
#
# Examples:
#   scripts/build_beagleplay.sh                      # drm-kms-vulkan, k3 image
#   scripts/build_beagleplay.sh --backend all
#   scripts/build_beagleplay.sh --with-vk-probe --deploy debian@beagleplay.local

set -euo pipefail

die()  { echo "error: $*" >&2; exit 1; }
log()  { echo "==> $*"; }
note() { [[ "${VERBOSE:-0}" -eq 1 ]] && echo "  · $*" || true; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

# ── Defaults ─────────────────────────────────────────────────────────────

BACKEND="drm-kms-vulkan"
KERNEL="k3"
ALL_BACKENDS=(wayland-egl wayland-vulkan drm-kms-egl drm-kms-vulkan software)
FLUTTER_ENGINE=""
FLUTTER_ENGINE_EXPLICIT=0
FLUTTER_RUNTIME="release"
FLUTTER_ENGINE_SHA=""
ENGINE_URL=""
PLUGINS_DIR="${REPO_DIR%/*}/ivi-homescreen-plugins/plugins"
NO_PLUGINS=0
WITH_SCENE=0
WITH_VK_PROBE=0
JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN=0
PREPARE_ONLY=0
REFRESH_SYSROOT=0
FORCE_SUDO=0       # --sudo-sysroot: skip the rootless path, use sudo loop-mount+chroot
ROOTLESS=0        # resolved in preflight: 1 = fuse2fs + unshare userns (no sudo)
IMAGE_URL=""
IMAGE_SHA256=""
TOOLCHAIN_URL=""
DEPLOY_HOST=""
IMAGE_SD_DEV=""    # --image-sd <dev>: flash the Debian image to a card (ERASES it)
STAGE_SD_DEV=""    # --stage-sd <dev>: offline-stage binary+engine+bundle onto a card
APP_BUNDLE=""      # --bundle <dir>: Flutter bundle (lib/libflutter_engine.so + data/)
PROVISION=0        # --provision: install + enable the systemd kiosk unit (masks the DM)
VERIFY_FLASH=1     # --no-verify-flash: skip the post-flash readback compare
DRM_DEVICE="/dev/dri/card0"
VERBOSE=0

# Debian 13 (trixie) → ARM GNU Toolchain 15.2.rel1 (gcc 15, glibc >= 2.41).
# Same pin + rationale as build_pi.sh's trixie target (libstdc++ gthr / glibc
# pthread_cond_t layout match). Override with --toolchain-version.
TC_TRIPLE="aarch64-none-linux-gnu"
TC_VERSION="15.2.rel1"
case "$(uname -m)" in
    aarch64|arm64) TC_HOST="aarch64" ;;
    *)             TC_HOST="x86_64" ;;
esac

# Pinned BeaglePlay Debian 13.5 images (BeagleBoard.org, 2026-05-19). The k3
# flavor carries the mainline 6.18 kernel + upstream PowerVR + Vulkan stack;
# the ti flavor is the older 6.12 TI BSP kernel. Update these (or pass
# --image-url / --image-sha256) as releases rotate.
IMG_BASE="https://files.beagle.cc/file/beagleboard-public-2021/images"
IMG_URL_K3="$IMG_BASE/beagleplay-debian-13.5-xfce-v6.18-k3-arm64-2026-05-19-12gb.img.xz"
IMG_SHA_K3="011b205f5fc2dbf9b68fed4c06ea530b55ac6f1c9f4df87973f5992940750e48"
IMG_URL_TI="$IMG_BASE/beagleplay-debian-13.5-xfce-v6.12-arm64-2026-05-19-12gb.img.xz"
IMG_SHA_TI="216991a354922fbef5b1d9d5d9bab518dfc67f791ebad2e5032d19ee8e6c4a5c"

# AM625 is 4x Cortex-A53.
XC_CPU_FLAGS="-mcpu=cortex-a53"

# Pinned Flutter engine SDK (github.com/meta-flutter/flutter-engine), arm64.
ENGINE_DEFAULT_SHA="13e658725ddaa270601426d1485636157e38c34c"
ENGINE_REPO="meta-flutter/flutter-engine"
ENGINE_ARCH="arm64"

# ── Argument parsing ─────────────────────────────────────────────────────

usage() {
    awk 'NR==1{next} /^[^#]/{exit} {sub(/^# ?/,""); print}' "$0"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --backend)            BACKEND="$2"; shift 2 ;;
        --kernel)             KERNEL="$2"; shift 2 ;;
        --flutter-engine)     FLUTTER_ENGINE="$2"; FLUTTER_ENGINE_EXPLICIT=1; shift 2 ;;
        --flutter-runtime)    FLUTTER_RUNTIME="$2"; shift 2 ;;
        --flutter-engine-sha) FLUTTER_ENGINE_SHA="$2"; shift 2 ;;
        --engine-url)         ENGINE_URL="$2"; shift 2 ;;
        --plugins-dir)        PLUGINS_DIR="$2"; shift 2 ;;
        --no-plugins)         NO_PLUGINS=1; shift ;;
        --with-scene)         WITH_SCENE=1; shift ;;
        --with-vk-probe)      WITH_VK_PROBE=1; shift ;;
        --jobs)               JOBS="$2"; shift 2 ;;
        --clean)              CLEAN=1; shift ;;
        --prepare-only)       PREPARE_ONLY=1; shift ;;
        --refresh-sysroot)    REFRESH_SYSROOT=1; shift ;;
        --sudo-sysroot)       FORCE_SUDO=1; shift ;;
        --image-url)          IMAGE_URL="$2"; shift 2 ;;
        --image-sha256)       IMAGE_SHA256="$2"; shift 2 ;;
        --toolchain-version)  TC_VERSION="$2"; shift 2 ;;
        --toolchain-url)      TOOLCHAIN_URL="$2"; shift 2 ;;
        --toolchain-host)     TC_HOST="$2"; shift 2 ;;
        --deploy)             DEPLOY_HOST="$2"; shift 2 ;;
        --image-sd)           IMAGE_SD_DEV="$2"; shift 2 ;;
        --stage-sd)           STAGE_SD_DEV="$2"; shift 2 ;;
        --bundle)             APP_BUNDLE="$2"; shift 2 ;;
        --provision)          PROVISION=1; shift ;;
        --no-verify-flash)    VERIFY_FLASH=0; shift ;;
        --drm-device)         DRM_DEVICE="$2"; shift 2 ;;
        -v|--verbose)         VERBOSE=1; shift ;;
        -h|--help)            usage 0 ;;
        *) die "unknown option: $1 (see --help)" ;;
    esac
done

case "$KERNEL" in k3|ti) ;; *) die "--kernel must be k3 or ti (got: $KERNEL)" ;; esac
case "$BACKEND" in
    wayland-egl|wayland-vulkan|drm-kms-egl|drm-kms-vulkan|software|all) ;;
    *) die "--backend must be wayland-egl|wayland-vulkan|drm-kms-egl|drm-kms-vulkan|software|all (got: $BACKEND)" ;;
esac
case "$FLUTTER_RUNTIME" in debug|debug-unopt|profile|release) ;;
    *) die "--flutter-runtime must be debug|debug-unopt|profile|release (got: $FLUTTER_RUNTIME)" ;;
esac

if [[ "$BACKEND" == "all" ]]; then
    BUILD_BACKENDS=("${ALL_BACKENDS[@]}")
else
    BUILD_BACKENDS=("$BACKEND")
fi

# Resolve image URL/checksum from the kernel flavor unless overridden.
if [[ -z "$IMAGE_URL" ]]; then
    case "$KERNEL" in
        k3) IMAGE_URL="$IMG_URL_K3"; [[ -z "$IMAGE_SHA256" ]] && IMAGE_SHA256="$IMG_SHA_K3" ;;
        ti) IMAGE_URL="$IMG_URL_TI"; [[ -z "$IMAGE_SHA256" ]] && IMAGE_SHA256="$IMG_SHA_TI" ;;
    esac
fi

[[ -z "$FLUTTER_ENGINE_SHA" ]] && FLUTTER_ENGINE_SHA="$ENGINE_DEFAULT_SHA"
ENGINE_TAG="linux-engine-sdk-${FLUTTER_RUNTIME}-${ENGINE_ARCH}-${FLUTTER_ENGINE_SHA}"
[[ -z "$ENGINE_URL" ]] \
    && ENGINE_URL="https://github.com/${ENGINE_REPO}/releases/download/${ENGINE_TAG}/${ENGINE_TAG}.tar.gz"

TC_TARBALL="arm-gnu-toolchain-${TC_VERSION}-${TC_HOST}-${TC_TRIPLE}.tar.xz"
TC_DIRNAME="arm-gnu-toolchain-${TC_VERSION}-${TC_HOST}-${TC_TRIPLE}"
[[ -z "$TOOLCHAIN_URL" ]] \
    && TOOLCHAIN_URL="https://developer.arm.com/-/media/Files/downloads/gnu/${TC_VERSION}/binrel/${TC_TARBALL}"

# ── Resolved sandbox paths ───────────────────────────────────────────────

XC_ROOT="${IVI_XC_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/ivi-homescreen-xc-beagleplay}"
DOWNLOADS="$XC_ROOT/downloads"
TC_DIR="$XC_ROOT/toolchain/$TC_DIRNAME"
CROSS_BIN="$TC_DIR/bin"
XC_SYSROOT="$XC_ROOT/sysroot/beagleplay"
ENGINE_CACHE_DIR="$XC_ROOT/flutter-engine/$ENGINE_TAG"
ENGINE_SDK_DIR="$ENGINE_CACHE_DIR/engine-sdk"

build_dir_for() { echo "$REPO_DIR/cmake-build-xc-beagleplay-$1"; }

export XC_ROOT CROSS_BIN XC_SYSROOT XC_CPU_FLAGS

# ── Phase 0: preflight ───────────────────────────────────────────────────

# True if the sysroot can be built with no sudo: rootless ext4 mount (fuse2fs)
# plus a usable unprivileged user namespace (fake-root for the qemu chroot apt).
rootless_capable() {
    command -v fuse2fs >/dev/null 2>&1 || return 1
    command -v unshare >/dev/null 2>&1 || return 1
    # The probe must actually enter a userns + mount /proc; some hardened hosts
    # enable the sysctl but block the mount.
    unshare -rmpf --mount-proc true >/dev/null 2>&1
}

# rm -rf that also clears root-owned leftovers from a prior sudo run: try as the
# caller first, then fall back to fake-root (userns when rootless, else sudo).
sysroot_rm() {
    local p="$1"
    [[ -e "$p" ]] || return 0
    rm -rf "$p" 2>/dev/null && return 0
    if [[ "$ROOTLESS" -eq 1 ]]; then unshare -rmpf rm -rf "$p"; else sudo rm -rf "$p"; fi
}

phase0_preflight() {
    log "Phase 0: preflight"
    local missing=()
    for tool in curl xz tar sfdisk rsync cmake pkg-config sha256sum file python3; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done
    command -v qemu-aarch64-static >/dev/null 2>&1 \
        || command -v /usr/bin/qemu-aarch64-static >/dev/null 2>&1 \
        || missing+=("qemu-aarch64-static (package: qemu-user-static)")
    if (( ${#missing[@]} )); then
        echo "missing host tools:" >&2
        printf '  - %s\n' "${missing[@]}" >&2
        echo "  Debian/Ubuntu: sudo apt install curl xz-utils tar fdisk rsync cmake pkg-config qemu-user-static python3 fuse2fs" >&2
        echo "  Fedora:        sudo dnf install curl xz tar util-linux rsync cmake pkgconf-pkg-config qemu-user-static python3 e2fsprogs" >&2
        exit 1
    fi

    # Decide sysroot mode: rootless (fuse2fs + userns) unless --sudo-sysroot or
    # the host can't do it. The sudo path additionally needs sudo + losetup.
    if [[ "$FORCE_SUDO" -eq 0 ]] && rootless_capable; then
        ROOTLESS=1
        note "sysroot   = rootless (fuse2fs + user namespace; no sudo)"
    else
        ROOTLESS=0
        local need=()
        command -v sudo     >/dev/null 2>&1 || need+=("sudo")
        command -v losetup  >/dev/null 2>&1 || need+=("losetup (package: util-linux)")
        if (( ${#need[@]} )); then
            echo "rootless sysroot unavailable and the sudo fallback is missing tools:" >&2
            printf '  - %s\n' "${need[@]}" >&2
            echo "  install fuse2fs (+ enable unprivileged user namespaces) for the rootless path," >&2
            echo "  or install sudo/util-linux for the privileged path." >&2
            exit 1
        fi
        if [[ "$FORCE_SUDO" -eq 1 ]]; then
            note "sysroot   = sudo loop-mount + chroot (--sudo-sysroot)"
        else
            note "sysroot   = sudo loop-mount + chroot (host lacks fuse2fs/userns)"
        fi
    fi

    mkdir -p "$DOWNLOADS" "$XC_ROOT/toolchain" "$XC_ROOT/sysroot" "$XC_ROOT/flutter-engine"
    note "XC_ROOT  = $XC_ROOT"
    note "kernel   = $KERNEL ($(basename "$IMAGE_URL"))"
    note "backends = ${BUILD_BACKENDS[*]}"
}

# ── Phase 1: toolchain ───────────────────────────────────────────────────

fetch() {
    local url="$1" dest="$2"
    if [[ -s "$dest" ]]; then note "cached: $(basename "$dest")"; return; fi
    log "fetching $(basename "$dest")"
    curl --fail --location --retry 3 --retry-delay 2 \
         --continue-at - --output "$dest.part" "$url"
    mv "$dest.part" "$dest"
}

verify_sha256() {
    local file="$1" want="$2" got
    [[ -n "$want" ]] || { note "no checksum for $(basename "$file"); skipping"; return; }
    got="$(sha256sum "$file" | awk '{print $1}')"
    [[ "$got" == "$want" ]] || die "sha256 mismatch for $file (got $got, want $want)"
    note "sha256 ok: $(basename "$file")"
}

verify_sha256_file() {
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
    if [[ -x "$CROSS_BIN/${TC_TRIPLE}-gcc" ]]; then note "toolchain present"; return; fi
    local tarball="$DOWNLOADS/$TC_TARBALL"
    fetch "$TOOLCHAIN_URL" "$tarball"
    verify_sha256_file "$tarball" "${TOOLCHAIN_URL}.sha256"
    log "extracting toolchain"
    tar -xJf "$tarball" -C "$XC_ROOT/toolchain"
    [[ -x "$CROSS_BIN/${TC_TRIPLE}-gcc" ]] \
        || die "toolchain extracted but $CROSS_BIN/${TC_TRIPLE}-gcc not found"
}

# ── Phase 1b: Flutter engine SDK (runtime artifact; dlopen'd, not linked) ─

phase1b_flutter_engine() {
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
        note "engine SDK present"; FLUTTER_ENGINE="$ENGINE_SDK_DIR"; return
    fi
    local tarball="$DOWNLOADS/${ENGINE_TAG}.tar.gz"
    fetch "$ENGINE_URL" "$tarball"
    verify_sha256_file "$tarball" "${ENGINE_URL}.sha256"
    log "extracting engine SDK → $ENGINE_CACHE_DIR"
    mkdir -p "$ENGINE_CACHE_DIR"
    tar -xzf "$tarball" -C "$ENGINE_CACHE_DIR" --strip-components=5
    [[ -f "$ENGINE_SDK_DIR/lib/libflutter_engine.so" ]] \
        || die "engine extracted but lib/libflutter_engine.so missing"
    FLUTTER_ENGINE="$ENGINE_SDK_DIR"
}

# ── Phase 2: sysroot ─────────────────────────────────────────────────────

relativize_symlinks() {
    local root="$1" link target rel
    while IFS= read -r link; do
        target="$(readlink "$link")"
        rel="$(python3 -c "import os,sys; print(os.path.relpath(sys.argv[1], os.path.dirname(sys.argv[2])))" \
                "${root}${target}" "$link")"
        ln -snf "$rel" "$link"
    done < <(find "$root" -type l -lname '/*')
}

# Stage the bits apt needs regardless of privilege model: the qemu interpreter,
# diversions that no-op initramfs/service starts, and a resolver (trixie ships
# resolv.conf as a /run stub symlink that dangles inside a chroot).
sysroot_apt_stage() {
    local qemu_bin f
    qemu_bin="$(command -v qemu-aarch64-static || echo /usr/bin/qemu-aarch64-static)"
    # The rootfs ships some of these (and a prior sudo run may have left them
    # root-owned). Remove first — `>` needs file-write, but we only own the
    # parent dir — then recreate as the caller.
    for f in usr/bin/qemu-aarch64-static usr/sbin/update-initramfs \
             usr/sbin/policy-rc.d etc/resolv.conf; do
        rm -f "$XC_SYSROOT/$f"
    done
    cp "$qemu_bin" "$XC_SYSROOT/usr/bin/qemu-aarch64-static"
    printf '#!/bin/sh\nexit 0\n'   > "$XC_SYSROOT/usr/sbin/update-initramfs"
    printf '#!/bin/sh\nexit 101\n' > "$XC_SYSROOT/usr/sbin/policy-rc.d"
    chmod +x "$XC_SYSROOT/usr/sbin/update-initramfs" "$XC_SYSROOT/usr/sbin/policy-rc.d"
    printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > "$XC_SYSROOT/etc/resolv.conf"
}

# Rootless -dev install: unshare -rmpf gives a private mount+pid namespace with
# the caller mapped to fake-root, so the bind mounts, chroot, and qemu apt run
# with no sudo and tear down automatically when the namespace exits.
sysroot_apt_install_rootless() {
    local pkgs=("$@")
    sysroot_apt_stage
    unshare -rmpf --mount-proc \
        env SYSROOT="$XC_SYSROOT" PKGS="${pkgs[*]}" sh -ec '
            # --rbind (recursive) the host /dev: a plain --bind fails on its
            # locked submounts (pts/shm), and a userns-mounted tmpfs is forced
            # nodev so its nodes are unusable. rbind keeps /dev/null et al. live.
            mount --rbind /dev "$SYSROOT/dev"
            # /proc and /sys are best-effort; the -dev packages do not require them.
            mount -t proc  proc  "$SYSROOT/proc" 2>/dev/null || true
            mount -t sysfs sysfs "$SYSROOT/sys"  2>/dev/null || true
            # APT::Sandbox::User=root: a single-uid userns has no _apt (uid 42),
            # so apt cannot drop privileges for downloads — keep it as root.
            Q=/usr/bin/qemu-aarch64-static
            A="-o APT::Sandbox::User=root"
            chroot "$SYSROOT" $Q /usr/bin/apt-get $A -y update
            chroot "$SYSROOT" $Q /usr/bin/apt-get $A -y install --no-install-recommends $PKGS
            chroot "$SYSROOT" $Q /usr/bin/apt-get $A -y clean
        ' || die "rootless apt install failed (try --sudo-sysroot)"
    rm -f "$XC_SYSROOT/usr/bin/qemu-aarch64-static"
}

sysroot_apt_install_sudo() {
    local pkgs=("$@")
    sysroot_apt_stage
    sudo mount --bind /dev      "$XC_SYSROOT/dev"
    sudo mount --bind /dev/pts  "$XC_SYSROOT/dev/pts"
    sudo mount -t proc  proc    "$XC_SYSROOT/proc"
    sudo mount -t sysfs sysfs   "$XC_SYSROOT/sys"
    trap "sudo umount -lq '$XC_SYSROOT/sys' '$XC_SYSROOT/proc' '$XC_SYSROOT/dev/pts' '$XC_SYSROOT/dev' 2>/dev/null || true; sudo rm -f '$XC_SYSROOT/usr/bin/qemu-aarch64-static'" EXIT
    sudo chroot "$XC_SYSROOT" /usr/bin/qemu-aarch64-static /usr/bin/apt-get -y update
    sudo chroot "$XC_SYSROOT" /usr/bin/qemu-aarch64-static /usr/bin/apt-get -y \
        install --no-install-recommends "${pkgs[@]}"
    sudo chroot "$XC_SYSROOT" /usr/bin/qemu-aarch64-static /usr/bin/apt-get -y clean
    sudo umount -lq "$XC_SYSROOT/sys" "$XC_SYSROOT/proc" "$XC_SYSROOT/dev/pts" "$XC_SYSROOT/dev"
    trap - EXIT
    sudo rm -f "$XC_SYSROOT/usr/bin/qemu-aarch64-static"
    sudo chown -R "$(id -u):$(id -g)" "$XC_SYSROOT"
}

sysroot_apt_install() {
    if [[ "$ROOTLESS" -eq 1 ]]; then
        sysroot_apt_install_rootless "$@"
    else
        sysroot_apt_install_sudo "$@"
    fi
}

# -dev packages across the queued backends. The Vulkan entry points are
# dlopen'd (vulkan.hpp) and the Vulkan headers are vendored, so drm-kms-vulkan
# needs no libvulkan-dev — only the DRM/GBM/seat/input stack (same as
# drm-kms-egl). libwayland-dev + wayland-protocols are shared (the
# wayland-cxx-scanner binding layer needs them). Debian trixie ships
# libdisplay-info 0.2.0 (>= drm-cxx's floor).
sysroot_pkg_list() {
    local pkgs=(
        libcamera-dev libcurl4-openssl-dev libegl-dev libgles2-mesa-dev
        libglib2.0-dev libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
        libjpeg-dev libpipewire-0.3-dev libsecret-1-dev libsystemd-dev
        libudev-dev libwayland-dev libxkbcommon-dev libxml2-dev
        wayland-protocols zlib1g-dev
    )
    local be
    for be in "${BUILD_BACKENDS[@]}"; do
        case "$be" in
            wayland-egl) : ;;
            wayland-vulkan) pkgs+=(libvulkan-dev mesa-vulkan-drivers) ;;
            drm-kms-egl|drm-kms-vulkan)
                pkgs+=(libdrm-dev libgbm-dev libinput-dev libxcursor-dev libseat-dev libdisplay-info-dev) ;;
            software) pkgs+=(libdrm-dev libinput-dev) ;;
        esac
    done
    printf '%s\n' "${pkgs[@]}"
}

# Post-apt fixups that must run on every path (fresh extract AND reused sysroot):
# re-relativize symlinks apt may have added, then restore the libc-merged dev
# symlinks that trixie's libc6-dev omits.
sysroot_finalize() {
    log "re-relativizing symlinks after apt"; relativize_symlinks "$XC_SYSROOT"
    # glibc 2.34+ merged librt/libdl/libpthread into libc; trixie's libc6-dev
    # drops the unversioned dev symlinks, breaking find_library(rt|dl|pthread).
    log "fixing up missing libc-merged dev symlinks"
    local ma="$XC_SYSROOT/usr/lib/aarch64-linux-gnu" stem sover
    for stem in rt dl pthread; do
        if [[ ! -e "$ma/lib${stem}.so" ]]; then
            sover="$(ls "$ma" | grep -E "^lib${stem}\.so\.[0-9]+$" | head -1)"
            [[ -n "$sover" ]] && ln -sf "$sover" "$ma/lib${stem}.so" && note "created lib${stem}.so -> $sover"
        fi
    done
}

phase2_sysroot() {
    log "Phase 2: sysroot (beagleplay, $KERNEL)"
    # Only treat the sysroot as reusable if it was actually populated; a bare
    # leftover dir from a failed run must fall through to a fresh extraction.
    if [[ -d "$XC_SYSROOT/usr/bin" && "$REFRESH_SYSROOT" -eq 0 ]]; then
        note "sysroot present; ensuring backend -dev packages are installed"
        local pkgs=(); mapfile -t pkgs < <(sysroot_pkg_list)
        sysroot_apt_install "${pkgs[@]}"
        sysroot_finalize; return
    fi
    # Empty/partial leftover → clear it so extraction starts clean.
    sysroot_rm "$XC_SYSROOT"
    [[ "$REFRESH_SYSROOT" -eq 1 ]] && { log "removing existing sysroot for refresh"; sysroot_rm "$XC_SYSROOT"; }

    local img_xz img
    img_xz="$DOWNLOADS/$(basename "$IMAGE_URL")"; img="${img_xz%.xz}"
    fetch "$IMAGE_URL" "$img_xz"
    verify_sha256 "$img_xz" "$IMAGE_SHA256"
    [[ -s "$img" ]] || { log "decompressing $(basename "$img_xz")"; xz -dkT0 "$img_xz"; }

    log "locating rootfs partition"
    local part_start part_offset
    part_start="$(sfdisk -J "$img" | python3 -c '
import json, sys
parts = json.load(sys.stdin)["partitiontable"]["partitions"]
linux = [p for p in parts if p.get("type") in ("83", "linux")]
# BeaglePlay images use a GUID/ext4 rootfs; fall back to the largest partition.
if not linux:
    linux = sorted(parts, key=lambda p: p.get("size", 0))[-1:]
if not linux: sys.exit("no rootfs partition")
print(linux[-1]["start"])')"
    part_offset=$(( part_start * 512 ))
    note "rootfs offset: $part_offset bytes"

    mkdir -p "$XC_SYSROOT"
    local mnt loop; mnt="$(mktemp -d)"
    if [[ "$ROOTLESS" -eq 1 ]]; then
        log "mounting rootfs (rootless, fuse2fs)"
        # fakeroot: bypass permission checks so we can read root-only files
        # (shadow, ssh host keys, /root) that the cross sysroot mirrors.
        fuse2fs -o ro,fakeroot,offset="$part_offset" "$img" "$mnt" 2>/dev/null \
            || die "fuse2fs could not mount the rootfs at offset $part_offset"
        trap "fusermount3 -u '$mnt' 2>/dev/null || fusermount -u '$mnt' 2>/dev/null; rmdir '$mnt' 2>/dev/null || true" EXIT
        log "rsyncing rootfs → $XC_SYSROOT"
        # --no-D: skip device/special nodes (can't be recreated unprivileged and
        # aren't needed in a cross sysroot). Files land owned by the caller.
        rsync -aH --no-D \
            --exclude=/proc/* --exclude=/sys/* --exclude=/dev/* --exclude=/run/* \
            --exclude=/tmp/* --exclude=/var/cache/apt/archives/* \
            "$mnt/" "$XC_SYSROOT/"
        fusermount3 -u "$mnt" 2>/dev/null || fusermount -u "$mnt" 2>/dev/null
        rmdir "$mnt"; trap - EXIT
    else
        log "mounting rootfs (sudo required)"
        loop="$(sudo losetup --show -f -P --offset "$part_offset" "$img")"
        trap "sudo umount -q '$mnt' || true; sudo losetup -d '$loop' || true; rmdir '$mnt' || true" EXIT
        sudo mount -o ro "$loop" "$mnt"
        log "rsyncing rootfs → $XC_SYSROOT"
        sudo rsync -aH --numeric-ids \
            --exclude=/proc/* --exclude=/sys/* --exclude=/dev/* --exclude=/run/* \
            --exclude=/tmp/* --exclude=/var/cache/apt/archives/* \
            "$mnt/" "$XC_SYSROOT/"
        sudo umount "$mnt"; sudo losetup -d "$loop"; rmdir "$mnt"; trap - EXIT
        sudo chown -R "$(id -u):$(id -g)" "$XC_SYSROOT" 2>/dev/null || true
    fi

    # Guard: if extraction produced nothing, fail here with a clear message
    # instead of letting the qemu-static copy later die with a confusing ENOENT.
    [[ -d "$XC_SYSROOT/usr/bin" ]] \
        || die "rootfs extraction produced an empty sysroot (no usr/bin) — aborting"

    log "relativizing absolute symlinks"; relativize_symlinks "$XC_SYSROOT"
    log "installing -dev packages via qemu-aarch64-static chroot"
    local pkgs=(); mapfile -t pkgs < <(sysroot_pkg_list)
    sysroot_apt_install "${pkgs[@]}"
    sysroot_finalize
}

# ── Phase 3: toolchain file + pkg-config wrapper ─────────────────────────

phase3_emit_cmake() {
    local be="$1" BUILD_DIR; BUILD_DIR="$(build_dir_for "$be")"
    log "Phase 3: emit toolchain file & pkg-config wrapper ($be)"
    mkdir -p "$BUILD_DIR"
    cat > "$BUILD_DIR/.xc-pkg-config" <<'EOF'
#!/bin/sh
# Generated by build_beagleplay.sh — sysroot-aware pkg-config wrapper.
export PKG_CONFIG_DIR=
export PKG_CONFIG_LIBDIR="$XC_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$XC_SYSROOT/usr/lib/pkgconfig:$XC_SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$XC_SYSROOT"
exec pkg-config "$@"
EOF
    chmod +x "$BUILD_DIR/.xc-pkg-config"
    cat > "$BUILD_DIR/.xc-toolchain.cmake" <<EOF
# Generated by scripts/build_beagleplay.sh — do not edit.
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT          "\$ENV{XC_SYSROOT}")
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
set(CMAKE_C_COMPILER       "\$ENV{CROSS_BIN}/${TC_TRIPLE}-gcc")
set(CMAKE_CXX_COMPILER     "\$ENV{CROSS_BIN}/${TC_TRIPLE}-g++")
set(CMAKE_FIND_ROOT_PATH   "\$ENV{XC_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(_xc_ma_inc "\${CMAKE_SYSROOT}/usr/include/aarch64-linux-gnu")
set(_xc_ma_usr "\${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu")
set(_xc_ma_lib "\${CMAKE_SYSROOT}/lib/aarch64-linux-gnu")
set(CMAKE_C_FLAGS_INIT     "-isystem \${_xc_ma_inc} -B\${_xc_ma_usr} \$ENV{XC_CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT   "-isystem \${_xc_ma_inc} -B\${_xc_ma_usr} \$ENV{XC_CPU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-L\${_xc_ma_usr} -L\${_xc_ma_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L\${_xc_ma_usr} -L\${_xc_ma_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-L\${_xc_ma_usr} -L\${_xc_ma_lib}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
set(PKG_CONFIG_EXECUTABLE  "$BUILD_DIR/.xc-pkg-config"
    CACHE FILEPATH "sysroot-aware pkg-config wrapper" FORCE)
EOF
    note "toolchain file: $BUILD_DIR/.xc-toolchain.cmake"
}

# ── Phase 4: configure & build ───────────────────────────────────────────

phase4_build() {
    local be="$1" BUILD_DIR; BUILD_DIR="$(build_dir_for "$be")"
    log "Phase 4: configure & build ($be)"
    local cmake_args=(
        -S "$REPO_DIR" -B "$BUILD_DIR"
        -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/.xc-toolchain.cmake"
        -DCMAKE_BUILD_TYPE=Release
        -DBUILD_BACKEND_DRM_KMS_VULKAN=OFF
    )
    case "$be" in
        wayland-egl)    cmake_args+=(-DBUILD_BACKEND_WAYLAND_EGL=ON -DBUILD_BACKEND_WAYLAND_VULKAN=OFF -DBUILD_BACKEND_DRM_KMS_EGL=OFF -DBUILD_BACKEND_SOFTWARE=OFF) ;;
        wayland-vulkan) cmake_args+=(-DBUILD_BACKEND_WAYLAND_EGL=OFF -DBUILD_BACKEND_WAYLAND_VULKAN=ON -DBUILD_BACKEND_DRM_KMS_EGL=OFF -DBUILD_BACKEND_SOFTWARE=OFF) ;;
        drm-kms-egl)
            cmake_args+=(-DBUILD_BACKEND_WAYLAND_EGL=OFF -DBUILD_BACKEND_WAYLAND_VULKAN=OFF -DBUILD_BACKEND_DRM_KMS_EGL=ON -DBUILD_BACKEND_SOFTWARE=OFF)
            [[ "$WITH_SCENE" -eq 1 ]] && cmake_args+=(-DBUILD_COMPOSITOR=ON -DUSE_DRM_SCENE=ON) ;;
        drm-kms-vulkan)
            # Flutter Vulkan renderer scanned out zero-copy on KMS planes via
            # dma-buf import. Requires the compositor (the engine presents
            # through CreateBackingStore/PresentLayers); the LayerScene path is
            # linked unconditionally, so USE_DRM_SCENE stays off.
            cmake_args+=(-DBUILD_BACKEND_WAYLAND_EGL=OFF -DBUILD_BACKEND_WAYLAND_VULKAN=OFF -DBUILD_BACKEND_DRM_KMS_EGL=OFF -DBUILD_BACKEND_DRM_KMS_VULKAN=ON -DBUILD_BACKEND_SOFTWARE=OFF -DBUILD_COMPOSITOR=ON) ;;
        software)       cmake_args+=(-DBUILD_BACKEND_WAYLAND_EGL=OFF -DBUILD_BACKEND_WAYLAND_VULKAN=OFF -DBUILD_BACKEND_DRM_KMS_EGL=OFF -DBUILD_BACKEND_SOFTWARE=ON) ;;
    esac
    [[ "$WITH_VK_PROBE" -eq 1 ]] && cmake_args+=(-DBUILD_DRM_KMS_VULKAN_PROBE=ON)
    if [[ "$NO_PLUGINS" -eq 1 ]]; then
        cmake_args+=(-DDISABLE_PLUGINS=ON)
    else
        [[ -d "$PLUGINS_DIR" ]] || die "plugins dir not found: $PLUGINS_DIR (use --no-plugins or --plugins-dir)"
        cmake_args+=(-DDISABLE_PLUGINS=OFF -DPLUGINS_DIR="$PLUGINS_DIR")
    fi
    log "cmake configure"; cmake "${cmake_args[@]}"
    log "cmake build (jobs=$JOBS)"; cmake --build "$BUILD_DIR" -j "$JOBS"
}

# ── Phase 5: report ──────────────────────────────────────────────────────

phase5_report() {
    log "Phase 5: artifacts"
    local be BUILD_DIR exe
    for be in "${BUILD_BACKENDS[@]}"; do
        BUILD_DIR="$(build_dir_for "$be")"; exe="$BUILD_DIR/shell/homescreen"
        echo "  [$be]"
        if [[ -x "$exe" ]]; then echo "    binary : $exe"; file "$exe" | sed 's/^/      /'
        else echo "    (no homescreen binary produced)"; fi
        [[ -x "$BUILD_DIR/shell/drm_kms_vulkan_probe" ]] && echo "    vk-probe : $BUILD_DIR/shell/drm_kms_vulkan_probe"
    done
    echo "  sysroot         : $XC_SYSROOT"
    echo "  toolchain       : $TC_DIR"
    echo "  engine (runtime): $FLUTTER_ENGINE"
    echo
    echo "  On-target Vulkan: the v6.18-k3 image carries the upstream PowerVR DRM"
    echo "  driver + Mesa pvr Vulkan. Confirm with 'vulkaninfo' (device = PowerVR"
    echo "  AXE). The drm_kms_vulkan backend additionally needs the dma-buf"
    echo "  scanout extensions (image_drm_format_modifier, external_memory_dma_buf,"
    echo "  queue_family_foreign, synchronization2, timeline) — run the vk-probe"
    echo "  (--with-vk-probe) on-target to verify before expecting a frame."
}

# ── Phase 6: optional deploy ─────────────────────────────────────────────

phase6_deploy() {
    [[ -n "$DEPLOY_HOST" ]] || return 0
    local be exe
    be="${BUILD_BACKENDS[0]}"
    exe="$(build_dir_for "$be")/shell/homescreen"
    [[ -x "$exe" ]] || die "no binary to deploy for backend $be"
    log "Phase 6: deploy $be binary → $DEPLOY_HOST"
    scp -q "$exe" "$DEPLOY_HOST:~/homescreen"
    echo
    echo "  Deployed ~/homescreen to $DEPLOY_HOST."
    echo "  Stage the engine + a bundle (engine is dlopen'd, not bundled in the"
    echo "  binary), then run on the BeaglePlay's console (DRM master needs a VT):"
    echo "    LD_LIBRARY_PATH=<bundle>/lib ./homescreen --drm-device $DRM_DEVICE -b <bundle>"
    echo "  Over SSH (no active VT), force the legacy direct-master session:"
    echo "    LIBSEAT_BACKEND=seatd LD_LIBRARY_PATH=<bundle>/lib \\"
    echo "      ./homescreen --drm-device $DRM_DEVICE -b <bundle>"
}

# Read the card back and byte-compare it to the image. Catches the failure mode
# where a flaky/counterfeit card (or reader) ACKs writes, drops off the USB bus
# mid-write, and leaves the tail (rootfs) unwritten — dd still exits 0. Buffers
# are flushed + caches dropped first so we compare the media, not the page cache.
verify_flash() {
    local dev="$1" img="$2" imgbytes
    imgbytes="$(stat -c %s "$img")"
    log "verifying flash: readback compare of $((imgbytes/1024/1024/1024)) GB (slow, but proves the card stored it)"
    sync
    sudo blockdev --flushbufs "$dev" 2>/dev/null || true
    sudo sh -c 'echo 3 > /proc/sys/vm/drop_caches' 2>/dev/null || true
    # cmp -n: compare exactly the image's length; it stops + reports the first
    # differing byte (which on a bad card lands right where the device dropped).
    if sudo cmp -n "$imgbytes" "$img" "$dev"; then
        log "flash verified OK — $imgbytes bytes on $dev match the image"
    else
        die "flash verification FAILED: the card did not store the image (the differing offset above is where it stopped). This is a failing/counterfeit card or a flaky reader — try a different card/reader."
    fi
}

# ── Flash the Debian image to a card ─────────────────────────────────────
# Writes the whole image (partition table + populated ext4 rootfs + TI boot
# blobs) to a removable device. This is the reliable way to populate a blank or
# half-flashed card — hand-partitioning the AM625 boot flow is error-prone.
phase_image_sd() {
    local dev="$IMAGE_SD_DEV"
    [[ -b "$dev" ]] || die "--image-sd: '$dev' is not a block device"
    # Safety: only removable media. Guards against a typo nuking a system disk.
    [[ "$(lsblk -dno RM "$dev" 2>/dev/null)" == "1" ]] \
        || die "--image-sd: $dev is not removable — refusing to erase it"
    [[ "$(lsblk -dno TYPE "$dev" 2>/dev/null)" == "disk" ]] \
        || die "--image-sd: $dev is not a whole disk (pass the device, not a partition)"

    mkdir -p "$DOWNLOADS"
    local img_xz img
    img_xz="$DOWNLOADS/$(basename "$IMAGE_URL")"; img="${img_xz%.xz}"
    fetch "$IMAGE_URL" "$img_xz"
    verify_sha256 "$img_xz" "$IMAGE_SHA256"
    [[ -s "$img" ]] || { log "decompressing $(basename "$img_xz")"; xz -dkT0 "$img_xz"; }

    log "Phase: flash $(basename "$img") → $dev  (sudo; ERASES $dev)"
    # Release the device: unmount auto-mounted partitions, disable any swap on it.
    local p
    while read -r p; do
        [[ -n "$p" ]] || continue
        findmnt -rno TARGET "/dev/$p" >/dev/null 2>&1 && { note "umount /dev/$p"; sudo umount "/dev/$p" || true; }
        grep -q "^/dev/$p " /proc/swaps 2>/dev/null && { note "swapoff /dev/$p"; sudo swapoff "/dev/$p" || true; }
    done < <(lsblk -lno NAME "$dev" | tail -n +2)

    sudo dd if="$img" of="$dev" bs=16M oflag=direct conv=fsync status=progress
    sync
    if [[ "$VERIFY_FLASH" -eq 1 ]]; then
        verify_flash "$dev" "$img"
    else
        note "skipping flash verification (--no-verify-flash)"
    fi
    sudo partprobe "$dev" 2>/dev/null || sudo blockdev --rereadpt "$dev" 2>/dev/null || true
    sudo udevadm settle 2>/dev/null || true
    log "flash complete: $dev"
}

# ── Phase 7: offline-stage onto a flashed SD card ────────────────────────
# Copies the binary + vk-probe + a Flutter bundle to /opt/ivi-homescreen on the
# card's rootfs. Needs sudo: the card is a physical block device (root:disk) with
# a root-owned system rootfs, so the rootless sysroot machinery does not apply.
phase7_stage_sd() {
    [[ -n "$STAGE_SD_DEV" ]] || return 0
    local dev="$STAGE_SD_DEV"
    [[ -b "$dev" ]] || die "--stage-sd: '$dev' is not a block device"

    local be exe probe
    be="${BUILD_BACKENDS[0]}"
    exe="$(build_dir_for "$be")/shell/homescreen"
    probe="$(build_dir_for "$be")/shell/drm_kms_vulkan_probe"
    [[ -x "$exe" ]] || die "no binary at $exe — run a build first (without --prepare-only)"

    [[ -n "$APP_BUNDLE" ]] || die "--stage-sd needs --bundle <dir>"
    [[ -d "$APP_BUNDLE/data/flutter_assets" ]] \
        || die "bundle missing data/flutter_assets: $APP_BUNDLE"
    [[ -f "$APP_BUNDLE/lib/libflutter_engine.so" ]] \
        || die "bundle missing lib/libflutter_engine.so: $APP_BUNDLE"

    log "Phase 7: stage → $dev (sudo mounts the card's rootfs)"
    # Rootfs = largest partition on the device (BeaglePlay: the ~10G ext4 root).
    local rootpart
    rootpart="$(lsblk -brno NAME,SIZE,TYPE "$dev" \
        | awk '$3=="part"{print $2"\t"$1}' | sort -n | tail -1 | cut -f2)"
    [[ -n "$rootpart" ]] || die "no partitions found on $dev"
    rootpart="/dev/$rootpart"
    note "rootfs partition: $rootpart"

    local mp; mp="$(mktemp -d)"
    trap "sudo umount -lq '$mp' 2>/dev/null; rmdir '$mp' 2>/dev/null || true" EXIT
    sudo mount "$rootpart" "$mp"
    [[ -d "$mp/etc" && -d "$mp/usr" ]] || die "$rootpart does not look like a rootfs"

    local dest="$mp/opt/ivi-homescreen"
    log "installing → /opt/ivi-homescreen"
    sudo install -d "$dest"
    sudo install -m0755 "$exe"   "$dest/homescreen"
    if [[ -x "$probe" ]]; then sudo install -m0755 "$probe" "$dest/drm_kms_vulkan_probe"; fi
    sudo rsync -a --delete "$APP_BUNDLE/" "$dest/bundle/"

    if [[ "$PROVISION" -eq 1 ]]; then
        log "provisioning systemd kiosk unit + masking the display manager"
        sudo tee "$mp/etc/systemd/system/ivi-homescreen.service" >/dev/null <<EOF
[Unit]
Description=ivi-homescreen (Flutter, $be)
After=systemd-user-sessions.service
Conflicts=getty@tty1.service

[Service]
Type=simple
Environment=LD_LIBRARY_PATH=/opt/ivi-homescreen/bundle/lib
Environment=XDG_RUNTIME_DIR=/run/user/0
# No seatd socket → homescreen falls back to direct /dev/dri open + drmSetMaster.
Environment=LIBSEAT_BACKEND=seatd
# NOTE: drm-kms-egl runs on software GL (llvmpipe) here, which renders into a
# tidss buffer that scans out. zink (GL-on-Vulkan, MESA_LOADER_DRIVER_OVERRIDE)
# would use the PowerVR GPU but allocates the buffer on the render node, which
# tidss cannot scan out (drmModeAddFB2 ENOENT → black screen) — so it is NOT set.
ExecStart=/opt/ivi-homescreen/homescreen --drm-device $DRM_DEVICE -b /opt/ivi-homescreen/bundle
Restart=on-failure
TTYPath=/dev/tty1
StandardInput=tty
StandardOutput=journal

[Install]
WantedBy=multi-user.target
EOF
        sudo mkdir -p "$mp/etc/systemd/system/multi-user.target.wants"
        sudo ln -sf ../ivi-homescreen.service \
            "$mp/etc/systemd/system/multi-user.target.wants/ivi-homescreen.service"
        # Free the GPU/VT: drop to multi-user and mask whatever DM is installed.
        sudo ln -sf multi-user.target "$mp/etc/systemd/system/default.target"
        local dm
        for dm in lightdm gdm gdm3 sddm; do
            [[ -e "$mp/etc/systemd/system/${dm}.service" || -e "$mp/lib/systemd/system/${dm}.service" ]] \
                && sudo ln -sf /dev/null "$mp/etc/systemd/system/${dm}.service"
        done
    fi

    sync; sudo umount "$mp"; rmdir "$mp"; trap - EXIT
    echo
    echo "  Staged to $rootpart:/opt/ivi-homescreen  (homescreen + bundle$([[ -x "$probe" ]] && echo " + vk-probe"))"
    if [[ "$PROVISION" -eq 1 ]]; then
        echo "  Kiosk service enabled; boots to multi-user and auto-starts on tty1."
    else
        echo "  Boot the board, then from a console VT (DRM master needs a free VT):"
        echo "    sudo systemctl stop lightdm   # free card0 if a desktop is running"
        echo "    LD_LIBRARY_PATH=/opt/ivi-homescreen/bundle/lib \\"
        echo "      /opt/ivi-homescreen/homescreen --drm-device $DRM_DEVICE -b /opt/ivi-homescreen/bundle"
        echo "  Check the Vulkan scanout gate first:  /opt/ivi-homescreen/drm_kms_vulkan_probe"
    fi
}

# ── Main ─────────────────────────────────────────────────────────────────

# Standalone card actions (flash and/or stage an existing build); no rebuild.
# Run them and exit before the build pipeline.
if [[ -n "$IMAGE_SD_DEV" ]]; then
    phase_image_sd
    # If a bundle was given, stage onto the freshly-flashed card in the same run.
    if [[ -n "$APP_BUNDLE" ]]; then STAGE_SD_DEV="$IMAGE_SD_DEV"; phase7_stage_sd; fi
    exit 0
fi
if [[ -n "$STAGE_SD_DEV" && "$PREPARE_ONLY" -eq 0 ]]; then
    phase7_stage_sd
    exit 0
fi

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

for be in "${BUILD_BACKENDS[@]}"; do
    [[ "$CLEAN" -eq 1 ]] && { log "wiping $(build_dir_for "$be")"; rm -rf "$(build_dir_for "$be")"; }
    phase3_emit_cmake "$be"
    phase4_build "$be"
done

phase5_report
phase6_deploy
