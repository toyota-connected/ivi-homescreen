#!/usr/bin/env bash
# Copyright 2026 Toyota Connected North America
#
# Sandboxed aarch64 cross-build of ivi-homescreen (+ ivi-homescreen-plugins)
# for the Radxa ZERO 3W and ZERO 3E (Rockchip RK3566, 4x Cortex-A55), modeled
# on scripts/build_beagleplay.sh: one aarch64 Debian sysroot extracted from the
# official SD-card image, an ARM GNU Toolchain, and a per-backend CMake build
# dir, plus SD-card flash + offline-stage helpers.
#
# One image, two boards
# ---------------------
# The ZERO 3W (Wi-Fi 6 / BT 5.4, USB-OTG) and the ZERO 3E (Gigabit Ethernet +
# PoE) are the SAME silicon — RK3566, Mali-G52 2EE GPU, VOP2 display — and ship
# from the SAME `radxa-build/radxa-zero3` image, which carries device trees for
# both and selects at boot. So the cross-build, the sysroot, and the flashed
# card are identical for the two boards. `--board zero3w|zero3e` only tunes the
# deploy/run guidance (which network path to reach the board on); it does not
# change what gets built or flashed.
#
# GPU / Vulkan stack — the thing this script exists to evaluate
# -------------------------------------------------------------
# The RK3566 integrates an Arm Mali-G52 2EE (Bifrost). Two userspace stacks
# exist and they differ sharply in what they can run:
#   * MAINLINE open stack (default on the b1 Bookworm image): the `panfrost`
#     DRM render driver + Mesa `panfrost` gives OpenGL ES 3.x; the Mesa `panvk`
#     Vulkan driver exists but is EXPERIMENTAL on Bifrost as of Mesa 24.x.
#   * Rockchip's proprietary `libmali` blob: GLES (+ advertised Vulkan 1.1) but
#     not the mainline KMS render-node path the DRM backends assume.
# Display is the Rockchip VOP2 (`rockchip` DRM driver, typically /dev/dri/card0;
# panfrost is the render node, renderD128).
#
# Because "which backend works best here" is an open question on this GPU, the
# default is --backend all: build every backend into its own dir so each can be
# flashed/run on the board and compared. Expect the GLES backends (wayland-egl,
# drm-kms-egl) to work via Panfrost; the Vulkan backends (wayland-vulkan,
# drm-kms-vulkan) hinge on PanVK maturity in the image's Mesa — build the
# vk-probe (--with-vk-probe) and run it on-target before expecting a frame.
#
# ivi-homescreen does NOT link libvulkan — vulkan.hpp dlopens it at runtime and
# the Vulkan headers are vendored — so the cross sysroot only needs the DRM /
# GBM / seat / input -dev packages. The Vulkan loader + the Mesa ICD (panvk /
# libmali) are RUNTIME requirements that ship in (or are apt-installed onto)
# the image.
#
# Debian release / libdisplay-info note
# -------------------------------------
# The pinned image is Debian 12 (Bookworm) → ARM GNU Toolchain 12.3.rel1
# (gcc 12, glibc 2.36; matches the target's libstdc++/glibc — see build_pi.sh).
# Bookworm ships libdisplay-info 0.1.1, which is BELOW drm-cxx's 0.2.0 floor, so
# the DRM backends (drm-kms-egl / drm-kms-vulkan) need libdisplay-info >= 0.2.0
# cross-built into the sysroot: pass --with-local-display-info (needs meson +
# ninja on the host). Without it, the DRM backends are skipped (for `all`) or
# refused (when requested explicitly).
#
# Sandbox layout (nothing is written under /usr, /opt, etc.):
#
#   $XC_ROOT/                       default: $XDG_CACHE_HOME/ivi-homescreen-xc-radxa-zero3
#     downloads/                    cached image + toolchain + engine tarballs
#     toolchain/arm-gnu-<ver>/      extracted ARM GNU Toolchain
#     sysroot/radxa-zero3/          Debian rootfs + apt-installed -dev packages
#     src/                          libdisplay-info / Vulkan-Headers source (optional)
#     flutter-engine/<tag>/         engine-sdk/ (lib/libflutter_engine.so, data/icudtl.dat)
#
#   <repo>/cmake-build-xc-radxa-zero3-<backend>/   CMake build dir (gitignored)
#
# Host requirements: x86_64 (or aarch64) Linux + qemu-user-static. The sysroot
# is built WITHOUT sudo when the host has fuse2fs and unprivileged user
# namespaces (rootless ext4 mount + fake-root qemu chroot); otherwise it falls
# back to a sudo loop-mount + chroot (as build_pi.sh does). Force the latter
# with --sudo-sysroot.
#
# Usage:
#   scripts/build_radxa_zero3.sh [options]
#     --board <zero3w|zero3e>       which board the deploy guidance targets
#                                   (default: zero3w; build/flash are identical)
#     --backend <wayland-egl|wayland-vulkan|drm-kms-egl|drm-kms-vulkan|software|all>
#                                   default: all (so every backend can be
#                                   compared on the board)
#     --flutter-engine <path>       dir with lib/libflutter_engine.so + data/icudtl.dat
#     --flutter-runtime <debug|debug-unopt|profile|release>   default: release
#     --flutter-engine-sha <sha>    pin a specific engine commit
#     --engine-url <url>            override the engine tarball URL
#     --plugins-dir <path>          default: ../ivi-homescreen-plugins/plugins
#     --no-plugins                  build homescreen only
#     --with-vk-probe               also build drm_kms_vulkan_probe (standalone
#                                   zero-copy dma-buf scanout capability probe;
#                                   run on-target to check the Mali Vulkan driver
#                                   exposes the required extensions)
#     --with-scene                  drm-kms-egl only: plane compositor + LayerScene
#     --with-local-display-info     cross-build libdisplay-info >= 0.2.0 from
#                                   source into the sysroot (static), enabling
#                                   the DRM backends on Bookworm (needs meson+ninja)
#     --display-info-version <ver>  libdisplay-info source tag (default 0.2.0)
#     --with-local-vulkan-headers   install newer Vulkan-Headers (header-only)
#                                   into the sysroot (for the Vulkan backends
#                                   against an older Vulkan SDK)
#     --vulkan-headers-version <t>  Vulkan-Headers tag (default vulkan-sdk-1.4.309.0)
#     --jobs <N>                    default: nproc
#     --clean                       wipe build dir before configure
#     --prepare-only                fetch/extract toolchain, sysroot, engine, exit
#     --refresh-sysroot             rebuild the sysroot from the image
#     --sudo-sysroot                force the sudo loop-mount + chroot path
#     --image-url <url>             override the Debian image URL
#     --image-sha512 <hex>          override the image checksum (sha512)
#     --toolchain-version <ver>     ARM GNU Toolchain version (default 12.3.rel1)
#     --toolchain-url <url>         override toolchain tarball URL
#     --toolchain-host <arch>       toolchain build-host arch (auto: x86_64/aarch64)
#
#   Deploy (post-build, optional):
#     --image-sd <dev>              flash the Debian image to a card (e.g.
#                                     /dev/sda), verifying its SHA512 first. ERASES
#                                     the device (removable-only). If --bundle is
#                                     also given, stages onto it after flashing —
#                                     so flash + stage is one command.
#     --verify-sd <dev>             read the card back and byte-compare it to the
#                                     image (no dd; read-only on the card, needs
#                                     sudo). Run standalone to check a card, OR
#                                     pass it ALONGSIDE --image-sd to flash THEN
#                                     verify in one pass (the readback runs right
#                                     after dd, before the card is auto-mounted —
#                                     the only point it still matches the image).
#                                     NOTE: a standalone verify of a card that has
#                                     been mounted/booted will differ (the OS
#                                     writes to it); verify right after flashing.
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
#     --deploy <user@host>          scp the built binary to the board and print
#                                     the run command. The Flutter engine + a
#                                     bundle are deployed separately by the
#                                     operator (engine is dlopen'd at runtime).
#     --drm-device <node>           DRM scanout node to suggest in the run hint
#                                     (default: /dev/dri/card0 — VOP2 display)
#
#     -v / --verbose
#     -h / --help
#
# Examples:
#   scripts/build_radxa_zero3.sh                              # all backends (GLES build, DRM skipped on Bookworm)
#   scripts/build_radxa_zero3.sh --with-local-display-info    # include the DRM backends too
#   scripts/build_radxa_zero3.sh --backend all --with-local-display-info --with-vk-probe
#   scripts/build_radxa_zero3.sh --image-sd /dev/sda --bundle <bundle> --provision

set -euo pipefail

die()  { echo "error: $*" >&2; exit 1; }
log()  { echo "==> $*"; }
note() { [[ "${VERBOSE:-0}" -eq 1 ]] && echo "  · $*" || true; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

# ── Defaults ─────────────────────────────────────────────────────────────

BOARD="zero3w"
BACKEND="all"
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
WITH_LOCAL_DISPLAY_INFO=0
LIBDISPLAY_INFO_VERSION="0.2.0"   # min for drm-cxx (HDR/colorimetry EDID APIs)
LIBDISPLAY_INFO_URL=""
WITH_LOCAL_VULKAN_HEADERS=0
VULKAN_HEADERS_VERSION="vulkan-sdk-1.4.309.0"  # VK_HEADER_VERSION 309
VULKAN_HEADERS_URL=""
JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN=0
PREPARE_ONLY=0
REFRESH_SYSROOT=0
FORCE_SUDO=0       # --sudo-sysroot: skip the rootless path, use sudo loop-mount+chroot
ROOTLESS=0        # resolved in preflight: 1 = fuse2fs + unshare userns (no sudo)
IMAGE_URL=""
IMAGE_SHA512=""
TOOLCHAIN_URL=""
DEPLOY_HOST=""
IMAGE_SD_DEV=""    # --image-sd <dev>: flash the Debian image to a card (ERASES it)
VERIFY_SD_DEV=""   # --verify-sd <dev>: non-destructive readback compare vs the image
STAGE_SD_DEV=""    # --stage-sd <dev>: offline-stage binary+engine+bundle onto a card
APP_BUNDLE=""      # --bundle <dir>: Flutter bundle (lib/libflutter_engine.so + data/)
PROVISION=0        # --provision: install + enable the systemd kiosk unit (masks the DM)
VERIFY_FLASH=1     # --no-verify-flash: skip the post-flash readback compare
DRM_DEVICE="/dev/dri/card0"
VERBOSE=0

# Debian 12 (bookworm) → ARM GNU Toolchain 12.3.rel1 (gcc 12, glibc 2.36).
# Same pin + rationale as build_pi.sh's bookworm target (libstdc++ gthr / glibc
# pthread_cond_t layout match). Override with --toolchain-version.
TC_TRIPLE="aarch64-none-linux-gnu"
TC_VERSION="12.3.rel1"
case "$(uname -m)" in
    aarch64|arm64) TC_HOST="aarch64" ;;
    *)             TC_HOST="x86_64" ;;
esac

# Pinned Radxa ZERO 3 image (radxa-build/radxa-zero3, release rsdk-b1). This is
# the Debian 12 (bookworm) KDE desktop image; it carries device trees for both
# the ZERO 3W and the ZERO 3E and boots either board. The release checksums
# with sha512 (a .sha512sum sidecar), not sha256. Update these (or pass
# --image-url / --image-sha512) as releases rotate — see
# https://github.com/radxa-build/radxa-zero3/releases
IMG_BASE="https://github.com/radxa-build/radxa-zero3/releases/download"
IMG_RELEASE="rsdk-b1"
IMG_NAME="radxa-zero3_bookworm_kde_b1.output_512.img.xz"
IMG_URL_DEFAULT="$IMG_BASE/$IMG_RELEASE/$IMG_NAME"
IMG_SHA512_DEFAULT="6f9f67df6f997bef41aac2cc568ebb4b7820216be1256a49ce472cb877684c08ad793ff726da3155a02f0eab60b2b2c9318168b3cf8fab81849ae91e8724f10d"

# RK3566 is 4x Cortex-A55.
XC_CPU_FLAGS="-mcpu=cortex-a55"

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
        --board)              BOARD="$2"; shift 2 ;;
        --backend)            BACKEND="$2"; shift 2 ;;
        --flutter-engine)     FLUTTER_ENGINE="$2"; FLUTTER_ENGINE_EXPLICIT=1; shift 2 ;;
        --flutter-runtime)    FLUTTER_RUNTIME="$2"; shift 2 ;;
        --flutter-engine-sha) FLUTTER_ENGINE_SHA="$2"; shift 2 ;;
        --engine-url)         ENGINE_URL="$2"; shift 2 ;;
        --plugins-dir)        PLUGINS_DIR="$2"; shift 2 ;;
        --no-plugins)         NO_PLUGINS=1; shift ;;
        --with-scene)         WITH_SCENE=1; shift ;;
        --with-vk-probe)      WITH_VK_PROBE=1; shift ;;
        --with-local-display-info)   WITH_LOCAL_DISPLAY_INFO=1; shift ;;
        --display-info-version)      LIBDISPLAY_INFO_VERSION="$2"; shift 2 ;;
        --with-local-vulkan-headers) WITH_LOCAL_VULKAN_HEADERS=1; shift ;;
        --vulkan-headers-version)    VULKAN_HEADERS_VERSION="$2"; shift 2 ;;
        --jobs)               JOBS="$2"; shift 2 ;;
        --clean)              CLEAN=1; shift ;;
        --prepare-only)       PREPARE_ONLY=1; shift ;;
        --refresh-sysroot)    REFRESH_SYSROOT=1; shift ;;
        --sudo-sysroot)       FORCE_SUDO=1; shift ;;
        --image-url)          IMAGE_URL="$2"; shift 2 ;;
        --image-sha512)       IMAGE_SHA512="$2"; shift 2 ;;
        --toolchain-version)  TC_VERSION="$2"; shift 2 ;;
        --toolchain-url)      TOOLCHAIN_URL="$2"; shift 2 ;;
        --toolchain-host)     TC_HOST="$2"; shift 2 ;;
        --deploy)             DEPLOY_HOST="$2"; shift 2 ;;
        --image-sd)           IMAGE_SD_DEV="$2"; shift 2 ;;
        --verify-sd)          VERIFY_SD_DEV="$2"; shift 2 ;;
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

case "$BOARD" in zero3w|zero3e) ;; *) die "--board must be zero3w or zero3e (got: $BOARD)" ;; esac
case "$BACKEND" in
    wayland-egl|wayland-vulkan|drm-kms-egl|drm-kms-vulkan|software|all) ;;
    *) die "--backend must be wayland-egl|wayland-vulkan|drm-kms-egl|drm-kms-vulkan|software|all (got: $BACKEND)" ;;
esac
case "$FLUTTER_RUNTIME" in debug|debug-unopt|profile|release) ;;
    *) die "--flutter-runtime must be debug|debug-unopt|profile|release (got: $FLUTTER_RUNTIME)" ;;
esac

# Resolve image URL/checksum from the pinned release unless overridden.
[[ -z "$IMAGE_URL" ]]    && IMAGE_URL="$IMG_URL_DEFAULT"
[[ -z "$IMAGE_SHA512" ]] && IMAGE_SHA512="$IMG_SHA512_DEFAULT"

[[ -z "$FLUTTER_ENGINE_SHA" ]] && FLUTTER_ENGINE_SHA="$ENGINE_DEFAULT_SHA"
ENGINE_TAG="linux-engine-sdk-${FLUTTER_RUNTIME}-${ENGINE_ARCH}-${FLUTTER_ENGINE_SHA}"
[[ -z "$ENGINE_URL" ]] \
    && ENGINE_URL="https://github.com/${ENGINE_REPO}/releases/download/${ENGINE_TAG}/${ENGINE_TAG}.tar.gz"

TC_TARBALL="arm-gnu-toolchain-${TC_VERSION}-${TC_HOST}-${TC_TRIPLE}.tar.xz"
TC_DIRNAME="arm-gnu-toolchain-${TC_VERSION}-${TC_HOST}-${TC_TRIPLE}"
[[ -z "$TOOLCHAIN_URL" ]] \
    && TOOLCHAIN_URL="https://developer.arm.com/-/media/Files/downloads/gnu/${TC_VERSION}/binrel/${TC_TARBALL}"

# libdisplay-info / Vulkan-Headers source archives (pinned by tag).
LIBDISPLAY_INFO_TARBALL="libdisplay-info-${LIBDISPLAY_INFO_VERSION}.tar.gz"
[[ -z "$LIBDISPLAY_INFO_URL" ]] \
    && LIBDISPLAY_INFO_URL="https://gitlab.freedesktop.org/emersion/libdisplay-info/-/archive/${LIBDISPLAY_INFO_VERSION}/${LIBDISPLAY_INFO_TARBALL}"
[[ -z "$VULKAN_HEADERS_URL" ]] \
    && VULKAN_HEADERS_URL="https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${VULKAN_HEADERS_VERSION}.tar.gz"

# ── Resolved sandbox paths ───────────────────────────────────────────────

XC_ROOT="${IVI_XC_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/ivi-homescreen-xc-radxa-zero3}"
DOWNLOADS="$XC_ROOT/downloads"
TC_DIR="$XC_ROOT/toolchain/$TC_DIRNAME"
CROSS_BIN="$TC_DIR/bin"
XC_SYSROOT="$XC_ROOT/sysroot/radxa-zero3"
ENGINE_CACHE_DIR="$XC_ROOT/flutter-engine/$ENGINE_TAG"
ENGINE_SDK_DIR="$ENGINE_CACHE_DIR/engine-sdk"

build_dir_for() { echo "$REPO_DIR/cmake-build-xc-radxa-zero3-$1"; }

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
    for tool in curl xz tar sfdisk rsync cmake pkg-config sha512sum file python3; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done
    command -v qemu-aarch64-static >/dev/null 2>&1 \
        || command -v /usr/bin/qemu-aarch64-static >/dev/null 2>&1 \
        || missing+=("qemu-aarch64-static (package: qemu-user-static)")
    if [[ "$WITH_LOCAL_DISPLAY_INFO" -eq 1 ]]; then
        for tool in meson ninja; do
            command -v "$tool" >/dev/null 2>&1 || missing+=("$tool (for --with-local-display-info)")
        done
    fi
    if (( ${#missing[@]} )); then
        echo "missing host tools:" >&2
        printf '  - %s\n' "${missing[@]}" >&2
        echo "  Debian/Ubuntu: sudo apt install curl xz-utils tar fdisk rsync cmake pkg-config qemu-user-static python3 fuse2fs meson ninja-build" >&2
        echo "  Fedora:        sudo dnf install curl xz tar util-linux rsync cmake pkgconf-pkg-config qemu-user-static python3 e2fsprogs meson ninja-build" >&2
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
    note "board    = $BOARD"
    note "image    = $(basename "$IMAGE_URL")"
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

verify_sha512() {
    local file="$1" want="$2" got
    [[ -n "$want" ]] || { note "no checksum for $(basename "$file"); skipping"; return; }
    got="$(sha512sum "$file" | awk '{print $1}')"
    [[ "$got" == "$want" ]] || die "sha512 mismatch for $file (got $got, want $want)"
    note "sha512 ok: $(basename "$file")"
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

# The Radxa image ships a VENDOR-FORKED Mesa + libdrm (radxa-repo, *~bpo12
# builds: libgbm1 25.0.7, libdrm2 2.4.123, …) whose runtime is already
# installed, so the stock Debian -dev packages cannot be installed — their
# strict "Depends: <runtime> (= <exact ver>)" conflicts with the fork (e.g.
# "libgbm-dev depends libgbm1 (= 22.3.6) but 25.0.7 is installed"). libdrm-dev
# pins SEVERAL siblings (libdrm2 + libdrm-{radeon,nouveau,amdgpu}1), and other
# -dev packages pull these transitively, so omitting them from the list doesn't
# help — apt deadlocks. The runtime fork already supplies the .so and the
# GBM/DRM APIs are stable across minor versions, so for these packages we
# download the stock -dev .deb, relax ALL exact-version pins in its control to
# unversioned, then APT-install the repacked .deb (apt, not dpkg, so any
# not-yet-present runtime sibling is pulled at the available forked version).
MESA_UNPIN_DEV=(libdrm-dev libgbm-dev)
APT_UNPIN=(); APT_STRICT=()

_in_list() { local x="$1"; shift; local e; for e in "$@"; do [[ "$e" == "$x" ]] && return 0; done; return 1; }

# The repacked .deb is built with the HOST's native dpkg-deb, not the chroot's:
# bookworm's `dpkg-deb --build` creates its members via O_TMPFILE, which
# qemu-user does not emulate (→ "failed to make temporary file: No such file or
# directory"). Extraction + the apt download/install stay in the chroot; only
# the rebuild crosses to the host (a .deb is arch-agnostic ar+tar, so the host
# tool repacks an arm64 package fine).

# Stage the bits apt needs regardless of privilege model: the qemu interpreter,
# diversions that no-op initramfs/service starts, and a resolver.
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
    sysroot_apt_stage
    local dpkgdeb; dpkgdeb="$(command -v dpkg-deb)"
    unshare -rmpf --mount-proc \
        env SYSROOT="$XC_SYSROOT" UNPIN="${APT_UNPIN[*]}" STRICT="${APT_STRICT[*]}" \
            DPKGDEB="$dpkgdeb" sh -ec '
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
            # Vendor-fork -dev packages: download in the chroot, repack on the
            # HOST (native dpkg-deb — the chroot one hits the qemu-user O_TMPFILE
            # bug), then apt-install the repacked .deb back in the chroot so apt
            # pulls any not-yet-present runtime siblings at the forked version.
            if [ -n "$UNPIN" ]; then
                D="$SYSROOT/tmp/ivi-devpkg"; rm -rf "$D"; mkdir -p "$D"
                for pkg in $UNPIN; do
                    echo "  unpin-install: $pkg (relax exact-version pins — vendor Mesa/libdrm fork in place)"
                    chroot "$SYSROOT" $Q /bin/sh -c "cd /tmp/ivi-devpkg && apt-get $A download $pkg"
                    deb=$(ls -1 "$D/${pkg}"_*.deb | head -1)
                    rm -rf "$D/x"
                    "$DPKGDEB" -R "$deb" "$D/x"
                    sed -i -E "s/ \(= [^)]*\)//g" "$D/x/DEBIAN/control"
                    "$DPKGDEB" -b "$D/x" "$D/${pkg}-repacked.deb"
                    chroot "$SYSROOT" $Q /usr/bin/apt-get $A -y --no-install-recommends \
                        install "/tmp/ivi-devpkg/${pkg}-repacked.deb"
                done
                rm -rf "$D"
            fi
            chroot "$SYSROOT" $Q /usr/bin/apt-get $A -y install --no-install-recommends $STRICT
            chroot "$SYSROOT" $Q /usr/bin/apt-get $A -y clean
        ' || die "rootless apt install failed (try --sudo-sysroot)"
    rm -f "$XC_SYSROOT/usr/bin/qemu-aarch64-static"
}

sysroot_apt_install_sudo() {
    sysroot_apt_stage
    local Q=/usr/bin/qemu-aarch64-static dpkgdeb; dpkgdeb="$(command -v dpkg-deb)"
    sudo mount --bind /dev      "$XC_SYSROOT/dev"
    sudo mount --bind /dev/pts  "$XC_SYSROOT/dev/pts"
    sudo mount -t proc  proc    "$XC_SYSROOT/proc"
    sudo mount -t sysfs sysfs   "$XC_SYSROOT/sys"
    trap "sudo umount -lq '$XC_SYSROOT/sys' '$XC_SYSROOT/proc' '$XC_SYSROOT/dev/pts' '$XC_SYSROOT/dev' 2>/dev/null || true; sudo rm -f '$XC_SYSROOT/usr/bin/qemu-aarch64-static'" EXIT
    sudo chroot "$XC_SYSROOT" "$Q" /usr/bin/apt-get -y update
    if (( ${#APT_UNPIN[@]} )); then
        local D="$XC_SYSROOT/tmp/ivi-devpkg" pkg deb
        sudo rm -rf "$D"; sudo mkdir -p "$D"
        for pkg in "${APT_UNPIN[@]}"; do
            echo "  unpin-install: $pkg (relax exact-version pins — vendor Mesa/libdrm fork in place)"
            sudo chroot "$XC_SYSROOT" "$Q" /bin/sh -c "cd /tmp/ivi-devpkg && apt-get download $pkg"
            deb="$(sudo sh -c "ls -1 '$D/${pkg}'_*.deb" | head -1)"
            sudo rm -rf "$D/x"
            sudo "$dpkgdeb" -R "$deb" "$D/x"
            sudo sed -i -E 's/ \(= [^)]*\)//g' "$D/x/DEBIAN/control"
            sudo "$dpkgdeb" -b "$D/x" "$D/${pkg}-repacked.deb"
            sudo chroot "$XC_SYSROOT" "$Q" /usr/bin/apt-get -y --no-install-recommends \
                install "/tmp/ivi-devpkg/${pkg}-repacked.deb"
        done
        sudo rm -rf "$D"
    fi
    sudo chroot "$XC_SYSROOT" "$Q" /usr/bin/apt-get -y \
        install --no-install-recommends "${APT_STRICT[@]}"
    sudo chroot "$XC_SYSROOT" "$Q" /usr/bin/apt-get -y clean
    sudo umount -lq "$XC_SYSROOT/sys" "$XC_SYSROOT/proc" "$XC_SYSROOT/dev/pts" "$XC_SYSROOT/dev"
    trap - EXIT
    sudo rm -f "$XC_SYSROOT/usr/bin/qemu-aarch64-static"
    sudo chown -R "$(id -u):$(id -g)" "$XC_SYSROOT"
}

# Split the requested -dev packages into the vendor-fork set (repacked with
# relaxed pins) and the rest (plain apt install), then dispatch by privilege.
# sysroot_pkg_list emits a name once per backend that needs it, so dedupe here
# (apt tolerates duplicate strict names, but the unpin loop would re-repack each).
sysroot_apt_install() {
    APT_UNPIN=(); APT_STRICT=()
    local p
    for p in "$@"; do
        if _in_list "$p" "${APT_UNPIN[@]}" || _in_list "$p" "${APT_STRICT[@]}"; then continue; fi
        if _in_list "$p" "${MESA_UNPIN_DEV[@]}"; then APT_UNPIN+=("$p"); else APT_STRICT+=("$p"); fi
    done
    if [[ "$ROOTLESS" -eq 1 ]]; then
        sysroot_apt_install_rootless
    else
        sysroot_apt_install_sudo
    fi
}

# -dev packages across the queued backends. The Vulkan entry points are
# dlopen'd (vulkan.hpp) and the Vulkan headers are vendored, so the Vulkan
# backends need no libvulkan-dev for the SHELL itself — only the DRM/GBM/seat/
# input stack (same as drm-kms-egl). libwayland-dev + wayland-protocols are
# shared (the wayland-cxx-scanner binding layer needs them). On Bookworm
# libdisplay-info-dev is 0.1.1; it is only apt-installed when NOT cross-building
# a newer one (--with-local-...).
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
                pkgs+=(libdrm-dev libgbm-dev libinput-dev libxcursor-dev libseat-dev)
                [[ "$WITH_LOCAL_DISPLAY_INFO" -eq 0 ]] && pkgs+=(libdisplay-info-dev) ;;
            software) pkgs+=(libdrm-dev libinput-dev) ;;
        esac
    done
    printf '%s\n' "${pkgs[@]}"
}

# Post-apt fixups that must run on every path (fresh extract AND reused sysroot):
# re-relativize symlinks apt may have added, then restore the libc-merged dev
# symlinks that newer libc6-dev omits.
sysroot_finalize() {
    log "re-relativizing symlinks after apt"; relativize_symlinks "$XC_SYSROOT"
    # glibc 2.34+ merged librt/libdl/libpthread into libc; recent libc6-dev
    # drops the unversioned dev symlinks, breaking find_library(rt|dl|pthread).
    log "fixing up missing libc-merged dev symlinks"
    local ma="$XC_SYSROOT/usr/lib/aarch64-linux-gnu" stem sover
    for stem in rt dl pthread; do
        if [[ ! -e "$ma/lib${stem}.so" ]]; then
            sover="$(ls "$ma" | grep -E "^lib${stem}\.so\.[0-9]+$" | head -1)"
            [[ -n "$sover" ]] && ln -sf "$sover" "$ma/lib${stem}.so" && note "created lib${stem}.so -> $sover"
        fi
    done
    # drm-cxx includes the kernel UAPI DRM headers as <drm/drm_mode.h>, i.e. from
    # /usr/include/drm/. On Debian that path is normally provided by
    # linux-libc-dev, but the Radxa image's vendor linux-libc-dev (6.1.140-1)
    # omits the drm/ UAPI subset. libdrm-dev bundles the same headers under
    # /usr/include/libdrm/, so expose them at the <drm/...> path with a symlink.
    local inc="$XC_SYSROOT/usr/include"
    if [[ ! -e "$inc/drm/drm_mode.h" && -e "$inc/libdrm/drm_mode.h" ]]; then
        ln -sfn libdrm "$inc/drm" && note "shimmed /usr/include/drm -> libdrm (vendor linux-libc-dev lacks drm/ UAPI)"
    fi
}

phase2_sysroot() {
    log "Phase 2: sysroot (radxa-zero3, bookworm)"
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
    verify_sha512 "$img_xz" "$IMAGE_SHA512"
    [[ -s "$img" ]] || { log "decompressing $(basename "$img_xz")"; xz -dkT0 "$img_xz"; }

    log "locating rootfs partition"
    local part_start part_offset
    part_start="$(sfdisk -J "$img" | python3 -c '
import json, sys
parts = json.load(sys.stdin)["partitiontable"]["partitions"]
linux = [p for p in parts if p.get("type") in ("83", "linux")]
# Rockchip GPT images carry the rootfs as a Linux-filesystem GUID partition
# (not type "83"); fall back to the largest partition, which is the ext4 root.
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

# ── Phase 2b/2c: optional local libdisplay-info / Vulkan-Headers ─────────

displayinfo_modversion() {
    PKG_CONFIG_LIBDIR="$XC_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$XC_SYSROOT/usr/lib/pkgconfig:$XC_SYSROOT/usr/share/pkgconfig" \
    PKG_CONFIG_SYSROOT_DIR="$XC_SYSROOT" \
        pkg-config --modversion libdisplay-info 2>/dev/null || echo 0
}
displayinfo_ge_floor() {
    [[ "$1" != "0" ]] && \
    [[ "$(printf '%s\n%s\n' "$LIBDISPLAY_INFO_VERSION" "$1" | sort -V | head -1)" == "$LIBDISPLAY_INFO_VERSION" ]]
}

phase2b_local_display_info() {
    [[ "$WITH_LOCAL_DISPLAY_INFO" -eq 1 ]] || return 0
    log "Phase 2b: local libdisplay-info (>= ${LIBDISPLAY_INFO_VERSION}, static)"

    local have; have="$(displayinfo_modversion)"
    if displayinfo_ge_floor "$have"; then
        note "sysroot already has libdisplay-info $have; skipping local build"; return
    fi

    local tarball src bld cross
    tarball="$DOWNLOADS/$LIBDISPLAY_INFO_TARBALL"
    src="$XC_ROOT/src/libdisplay-info-${LIBDISPLAY_INFO_VERSION}"
    bld="$src/build-xc-radxa-zero3"
    cross="$src/.xc-cross-radxa-zero3.ini"
    fetch "$LIBDISPLAY_INFO_URL" "$tarball"

    if [[ ! -f "$src/meson.build" ]]; then
        log "extracting libdisplay-info"
        mkdir -p "$XC_ROOT/src"; rm -rf "$src"
        local tmp; tmp="$(mktemp -d "$XC_ROOT/src/.unpack.XXXXXX")"
        tar -xzf "$tarball" -C "$tmp"; mv "$tmp"/*/ "$src"; rmdir "$tmp"
    fi

    local ma_inc="$XC_SYSROOT/usr/include/aarch64-linux-gnu"
    local ma_usr="$XC_SYSROOT/usr/lib/aarch64-linux-gnu"
    local ma_lib="$XC_SYSROOT/lib/aarch64-linux-gnu"
    cat > "$cross" <<EOF
[binaries]
c = '$CROSS_BIN/${TC_TRIPLE}-gcc'
cpp = '$CROSS_BIN/${TC_TRIPLE}-g++'
ar = '$CROSS_BIN/${TC_TRIPLE}-ar'
strip = '$CROSS_BIN/${TC_TRIPLE}-strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[built-in options]
c_args = ['--sysroot=$XC_SYSROOT', '-isystem', '$ma_inc', '-B$ma_usr'${XC_CPU_FLAGS:+, '$XC_CPU_FLAGS'}]
c_link_args = ['--sysroot=$XC_SYSROOT', '-B$ma_usr', '-L$ma_usr', '-L$ma_lib']
EOF

    log "configuring + building libdisplay-info (meson cross, static)"
    rm -rf "$bld"
    PKG_CONFIG_LIBDIR="$ma_usr/pkgconfig:$XC_SYSROOT/usr/lib/pkgconfig:$XC_SYSROOT/usr/share/pkgconfig" \
    PKG_CONFIG_SYSROOT_DIR="$XC_SYSROOT" \
        meson setup "$bld" "$src" --cross-file "$cross" \
            --prefix /usr --libdir lib/aarch64-linux-gnu \
            --buildtype release --default-library static
    ninja -C "$bld"
    DESTDIR="$XC_SYSROOT" ninja -C "$bld" install
    rm -f "$ma_usr/libdisplay-info.so"

    local now; now="$(displayinfo_modversion)"
    displayinfo_ge_floor "$now" \
        || die "libdisplay-info install did not yield >= $LIBDISPLAY_INFO_VERSION (got '$now')"
    note "libdisplay-info $now installed (static) into radxa-zero3 sysroot"
}

vulkan_header_version() {
    local h="$XC_SYSROOT/usr/include/vulkan/vulkan_core.h"
    [[ -f "$h" ]] && awk '/#define VK_HEADER_VERSION /{print $3; exit}' "$h" || echo 0
}

phase2c_local_vulkan_headers() {
    [[ "$WITH_LOCAL_VULKAN_HEADERS" -eq 1 ]] || return 0
    local want; want="$(echo "$VULKAN_HEADERS_VERSION" | awk -F. '{print $3}')"
    log "Phase 2c: local Vulkan-Headers (VK_HEADER_VERSION >= ${want})"

    local have; have="$(vulkan_header_version)"
    if [[ "$have" =~ ^[0-9]+$ ]] && (( have >= want )); then
        note "sysroot already has VK_HEADER_VERSION $have; skipping"; return
    fi

    local tarball src
    tarball="$DOWNLOADS/Vulkan-Headers-${VULKAN_HEADERS_VERSION}.tar.gz"
    src="$XC_ROOT/src/Vulkan-Headers-${VULKAN_HEADERS_VERSION}"
    fetch "$VULKAN_HEADERS_URL" "$tarball"

    if [[ ! -d "$src/include/vulkan" ]]; then
        log "extracting Vulkan-Headers"
        mkdir -p "$XC_ROOT/src"; rm -rf "$src"
        local tmp; tmp="$(mktemp -d "$XC_ROOT/src/.unpack.XXXXXX")"
        tar -xzf "$tarball" -C "$tmp"; mv "$tmp"/*/ "$src"; rmdir "$tmp"
    fi

    log "installing Vulkan-Headers into sysroot (header-only)"
    mkdir -p "$XC_SYSROOT/usr/include/vulkan" "$XC_SYSROOT/usr/include/vk_video"
    cp -a "$src/include/vulkan/." "$XC_SYSROOT/usr/include/vulkan/"
    [[ -d "$src/include/vk_video" ]] \
        && cp -a "$src/include/vk_video/." "$XC_SYSROOT/usr/include/vk_video/"
    note "Vulkan-Headers now VK_HEADER_VERSION $(vulkan_header_version) in radxa-zero3 sysroot"
}

# ── Phase 3: toolchain file + pkg-config wrapper ─────────────────────────

phase3_emit_cmake() {
    local be="$1" BUILD_DIR; BUILD_DIR="$(build_dir_for "$be")"
    log "Phase 3: emit toolchain file & pkg-config wrapper ($be)"
    mkdir -p "$BUILD_DIR"
    cat > "$BUILD_DIR/.xc-pkg-config" <<'EOF'
#!/bin/sh
# Generated by build_radxa_zero3.sh — sysroot-aware pkg-config wrapper.
export PKG_CONFIG_DIR=
export PKG_CONFIG_LIBDIR="$XC_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$XC_SYSROOT/usr/lib/pkgconfig:$XC_SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$XC_SYSROOT"
exec pkg-config "$@"
EOF
    chmod +x "$BUILD_DIR/.xc-pkg-config"
    cat > "$BUILD_DIR/.xc-toolchain.cmake" <<EOF
# Generated by scripts/build_radxa_zero3.sh — do not edit.
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
            [[ "$WITH_SCENE" -eq 1 ]] && cmake_args+=(-DBUILD_COMPOSITOR=ON) ;;
        drm-kms-vulkan)
            # Flutter Vulkan renderer scanned out zero-copy on KMS planes via
            # dma-buf import. Requires the compositor (the engine presents
            # through CreateBackingStore/PresentLayers).
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
    echo "  Which backend works best? Run each built binary on the board and compare."
    echo "  Mali-G52 (RK3566) GPU stack:"
    echo "    * wayland-egl / drm-kms-egl  → GLES via mainline Panfrost (most likely"
    echo "        to render; the b1 Bookworm image defaults to Panfrost)."
    echo "    * wayland-vulkan / drm-kms-vulkan → Vulkan via Mesa PanVK, which is"
    echo "        EXPERIMENTAL on Bifrost. Build --with-vk-probe and run the probe"
    echo "        on-target (dma-buf scanout: image_drm_format_modifier,"
    echo "        external_memory_dma_buf, queue_family_foreign, synchronization2,"
    echo "        timeline) before expecting a frame."
    echo "    * software → CPU rasterizer → VOP2 dumb buffer (always works; the"
    echo "        baseline to confirm the display path independent of the GPU)."
    if [[ "$WITH_LOCAL_DISPLAY_INFO" -eq 0 ]]; then
        echo "  NOTE: the DRM backends are skipped on this Bookworm sysroot"
        echo "        (libdisplay-info < ${LIBDISPLAY_INFO_VERSION}). Re-run with"
        echo "        --with-local-display-info to include them."
    fi
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
    echo "  Deployed ~/homescreen to $DEPLOY_HOST ($BOARD)."
    echo "  Stage the engine + a bundle (engine is dlopen'd, not bundled in the"
    echo "  binary), then run on the board's console (DRM master needs a VT):"
    echo "    LD_LIBRARY_PATH=<bundle>/lib ./homescreen --drm-device $DRM_DEVICE -b <bundle>"
    echo "  Over SSH (no active VT), force the legacy direct-master session:"
    echo "    LIBSEAT_BACKEND=seatd LD_LIBRARY_PATH=<bundle>/lib \\"
    echo "      ./homescreen --drm-device $DRM_DEVICE -b <bundle>"
}

# Prime sudo with a VISIBLE password prompt up front. Several sudo calls in the
# card phases redirect stderr to /dev/null to mute tool noise — which also hides
# sudo's own password prompt, leaving the script apparently hung while it
# silently waits for input on the tty. Validating credentials first means those
# later calls hit the cached timestamp and never prompt.
sudo_prime() {
    command -v sudo >/dev/null 2>&1 || die "sudo is required for SD-card operations but was not found"
    log "sudo: authenticating for SD-card access (password prompt follows)…"
    sudo -v || die "sudo authentication failed"
}

# Release a device: unmount any (auto-)mounted partitions + swapoff. A desktop
# auto-mounter grabs the BOOT vfat the moment the partition table reappears, and
# a mounted FAT has its dirty-bit / FSINFO rewritten — which both corrupts a dd
# in progress AND makes a later readback compare spuriously differ from the
# image. Call this before writing and before verifying.
release_device() {
    local dev="$1" p
    while read -r p; do
        [[ -n "$p" ]] || continue
        findmnt -rno TARGET "/dev/$p" >/dev/null 2>&1 && { note "umount /dev/$p"; sudo umount "/dev/$p" || true; }
        grep -q "^/dev/$p " /proc/swaps 2>/dev/null && { note "swapoff /dev/$p"; sudo swapoff "/dev/$p" || true; }
    done < <(lsblk -lno NAME "$dev" | tail -n +2)
    # Always succeed: the loop's last command (the swaps grep) returns non-zero
    # when the final partition isn't swap, which under `set -e` would abort the
    # caller at `release_device "$dev"`.
    return 0
}

# Read the card back and byte-compare it to the image. Catches the failure mode
# where a flaky/counterfeit card (or reader) ACKs writes, drops off the USB bus
# mid-write, and leaves the tail (rootfs) unwritten — dd still exits 0. We FIRST
# unmount any auto-mounted partition (else the desktop's touch of the BOOT vfat
# makes a pristine card read back as "different"), then flush buffers + drop
# caches so we compare the media, not the page cache.
verify_flash() {
    local dev="$1" img="$2" imgbytes
    imgbytes="$(stat -c %s "$img")"
    log "verifying flash: readback compare of $((imgbytes/1024/1024/1024)) GB (slow, but proves the card stored it)"
    release_device "$dev"
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

# ── Standalone: verify an already-flashed card against the image ─────────
# Non-destructive (no dd) readback compare, re-runnable. Use after --image-sd
# to re-confirm a card actually stored the image, or to check a card that was
# flashed elsewhere (e.g. via the leading `!` shell, or another tool). Reads
# the card read-only; the sudo is only for raw block access + cache drop.
phase_verify_sd() {
    local dev="$VERIFY_SD_DEV"
    [[ -b "$dev" ]] || die "--verify-sd: '$dev' is not a block device"
    [[ "$(lsblk -dno TYPE "$dev" 2>/dev/null)" == "disk" ]] \
        || die "--verify-sd: $dev is not a whole disk (pass the device, not a partition)"

    mkdir -p "$DOWNLOADS"
    local img_xz img
    img_xz="$DOWNLOADS/$(basename "$IMAGE_URL")"; img="${img_xz%.xz}"
    fetch "$IMAGE_URL" "$img_xz"
    verify_sha512 "$img_xz" "$IMAGE_SHA512"
    [[ -s "$img" ]] || { log "decompressing $(basename "$img_xz")"; xz -dkT0 "$img_xz"; }

    log "Phase: verify $dev against $(basename "$img")  (read-only; needs sudo)"
    sudo_prime
    verify_flash "$dev" "$img"
}

# ── Flash the Debian image to a card ─────────────────────────────────────
# Writes the whole image (GPT + idbloader/u-boot blobs + populated ext4 rootfs)
# to a removable device. This is the reliable way to populate a blank or
# half-flashed card — hand-partitioning the Rockchip boot flow is error-prone.
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
    verify_sha512 "$img_xz" "$IMAGE_SHA512"
    [[ -s "$img" ]] || { log "decompressing $(basename "$img_xz")"; xz -dkT0 "$img_xz"; }

    log "Phase: flash $(basename "$img") → $dev  (sudo; ERASES $dev)"
    sudo_prime
    release_device "$dev"
    # Kernel ≥5.x refuses to open a whole-disk device for writing while any of
    # its partitions are mounted (EBUSY), and the desktop auto-mounter (udisks)
    # re-grabs a partition faster than release_device can unmount it. Wiping the
    # partition signatures leaves nothing to auto-mount, so the dd below opens
    # cleanly. wipefs -a needs the device not busy → release first, wipe, settle,
    # then release once more to catch anything mounted during the wipe.
    sudo wipefs -a "$dev" >/dev/null 2>&1 || true
    sudo blockdev --rereadpt "$dev" 2>/dev/null || true
    sudo udevadm settle 2>/dev/null || true
    release_device "$dev"

    # Write with O_DIRECT first (writes hit the media, so status=progress is
    # honest and a verify reflects reality). Cheap USB card readers often throw
    # EIO under sustained O_DIRECT; fall back once to a kernel-paced buffered
    # write with smaller blocks before giving up.
    log "writing image (dd, O_DIRECT; the slow part)…"
    if ! sudo dd if="$img" of="$dev" bs=4M oflag=direct conv=fsync status=progress; then
        echo "  · O_DIRECT write failed (common on flaky USB readers) — retrying buffered" >&2
        release_device "$dev"
        sudo wipefs -a "$dev" >/dev/null 2>&1 || true
        sudo blockdev --rereadpt "$dev" 2>/dev/null || true
        sudo udevadm settle 2>/dev/null || true
        release_device "$dev"
        log "writing image (dd, buffered)…"
        sudo dd if="$img" of="$dev" bs=1M conv=fsync,fdatasync status=progress \
            || die "dd failed to write $dev even buffered (I/O error above). This is almost certainly a FAILING CARD or a FLAKY USB READER — try a different card and/or reader, and a direct USB port (no hub). It is not a script problem: dd reached the media and the device errored mid-write."
    fi
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
    sudo_prime
    # Rootfs = largest partition on the device (Rockchip: the big ext4 root).
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
Description=ivi-homescreen (Flutter, $be — $BOARD)
After=systemd-user-sessions.service
Conflicts=getty@tty1.service

[Service]
Type=simple
Environment=LD_LIBRARY_PATH=/opt/ivi-homescreen/bundle/lib
Environment=XDG_RUNTIME_DIR=/run/user/0
# No seatd socket → homescreen falls back to direct /dev/dri open + drmSetMaster.
Environment=LIBSEAT_BACKEND=seatd
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
        echo "    sudo systemctl stop sddm   # free card0 if the KDE desktop is running"
        echo "    LD_LIBRARY_PATH=/opt/ivi-homescreen/bundle/lib \\"
        echo "      /opt/ivi-homescreen/homescreen --drm-device $DRM_DEVICE -b /opt/ivi-homescreen/bundle"
        echo "  Check the Vulkan scanout gate first:  /opt/ivi-homescreen/drm_kms_vulkan_probe"
    fi
}

# ── Main ─────────────────────────────────────────────────────────────────

# The DRM backends need libdisplay-info >= the drm-cxx floor. Bookworm ships
# 0.1.1, so without --with-local-display-info they're dropped (for `all`) or
# refused (when requested explicitly). Resolved AFTER the sysroot/local builds
# exist so the version check sees the real sysroot.
resolve_backends() {
    local have; have="$(displayinfo_modversion)"
    if displayinfo_ge_floor "$have"; then return; fi   # floor met → keep all
    if [[ "$BACKEND" != "all" ]]; then
        if [[ "$BACKEND" == drm-kms-egl || "$BACKEND" == drm-kms-vulkan ]]; then
            die "$BACKEND needs libdisplay-info >= ${LIBDISPLAY_INFO_VERSION} (sysroot has '$have'); pass --with-local-display-info to cross-build it"
        fi
        return
    fi
    log "note: sysroot libdisplay-info '$have' < ${LIBDISPLAY_INFO_VERSION}; skipping the DRM backends (pass --with-local-display-info to include them)"
    local be kept=()
    for be in "${BUILD_BACKENDS[@]}"; do
        [[ "$be" == drm-kms-egl || "$be" == drm-kms-vulkan ]] && continue
        kept+=("$be")
    done
    BUILD_BACKENDS=("${kept[@]}")
}

# Seed BUILD_BACKENDS before the sysroot phases (sysroot_pkg_list reads it);
# resolve_backends refines it once the sysroot's libdisplay-info is known.
if [[ "$BACKEND" == "all" ]]; then
    BUILD_BACKENDS=("${ALL_BACKENDS[@]}")
else
    BUILD_BACKENDS=("$BACKEND")
fi

# Standalone card actions (flash, verify, and/or stage an existing build); no
# rebuild. Run them and exit before the build pipeline. Flashing takes
# precedence over verifying so that `--image-sd <dev> --verify-sd <dev>` means
# "write THEN read back", not "verify only". The readback then runs INSIDE the
# flash phase — right after dd, before partprobe lets the desktop auto-mount the
# card — which is the only point the media still byte-matches the image; so
# --verify-sd here just forces that in-flash readback on (it is the default
# anyway, unless --no-verify-flash).
if [[ -n "$IMAGE_SD_DEV" ]]; then
    [[ -n "$VERIFY_SD_DEV" ]] && VERIFY_FLASH=1
    phase_image_sd
    # If a bundle was given, stage onto the freshly-flashed card in the same run.
    if [[ -n "$APP_BUNDLE" ]]; then STAGE_SD_DEV="$IMAGE_SD_DEV"; phase7_stage_sd; fi
    exit 0
fi
if [[ -n "$VERIFY_SD_DEV" ]]; then
    phase_verify_sd
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
phase2b_local_display_info
phase2c_local_vulkan_headers
resolve_backends

if [[ "$PREPARE_ONLY" -eq 1 ]]; then
    log "prepare-only: stopping before configure"
    echo "  toolchain: $TC_DIR"
    echo "  sysroot  : $XC_SYSROOT"
    echo "  engine   : $FLUTTER_ENGINE"
    echo "  backends : ${BUILD_BACKENDS[*]}"
    exit 0
fi

for be in "${BUILD_BACKENDS[@]}"; do
    [[ "$CLEAN" -eq 1 ]] && { log "wiping $(build_dir_for "$be")"; rm -rf "$(build_dir_for "$be")"; }
    phase3_emit_cmake "$be"
    phase4_build "$be"
done

phase5_report
phase6_deploy