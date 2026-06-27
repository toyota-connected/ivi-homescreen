#!/usr/bin/env bash
# Copyright 2026 Toyota Connected North America
#
# Sandboxed aarch64 cross-build of ivi-homescreen (+ ivi-homescreen-plugins)
# for the Arduino UNO Q (Qualcomm Dragonwing QRB2210 — quad-core Cortex-A53,
# Adreno 702 GPU, Debian userspace, 16 GB eMMC).
#
# Unlike build_pi.sh, there is no public loop-mountable OS image and the board
# boots from eMMC (not a removable SD card). So this script:
#
#   * builds its cross sysroot by rsync'ing the rootfs off a LIVE, networked
#     UNO Q over SSH (then apt-installs the -dev packages into that copy via a
#     qemu-aarch64-static chroot, exactly like build_pi.sh), and
#   * deploys by scp'ing the built binary + Flutter bundle onto the running
#     board over SSH and installing a kiosk systemd unit there — no dd, no
#     fastboot.
#
# The cross-compiler, engine fetch, CMake toolchain-file emission, and the
# per-backend build loop are otherwise identical to build_pi.sh.
#
# Sandbox layout (nothing is written under /usr, /opt, etc.):
#
#   $XC_ROOT/                       default: $XDG_CACHE_HOME/ivi-homescreen-xc-unoq
#     downloads/                    cached toolchain + engine tarballs
#     toolchain/arm-gnu-<ver>/      extracted ARM GNU Toolchain
#     sysroot/unoq/                 rootfs rsync'd off the board + apt -dev pkgs
#     flutter-engine/<tag>/         engine-sdk/ (lib/libflutter_engine.so, …)
#
#   <repo>/cmake-build-xc-unoq-<backend>/   CMake build dir (gitignored)
#
# Host requirements: x86_64 (or aarch64) Linux. SSH access to the UNO Q.
#
# Usage:
#   scripts/build_unoq.sh [options]
#     --device-host <user@host>     SSH target of the running UNO Q. Required
#                                     for sysroot sync (--refresh-sysroot / first
#                                     run) and for --deploy. e.g. arduino@unoq.local
#     --ssh-port <n>                SSH port (default: 22)
#     --ssh-opts <str>              extra ssh/rsync -e options (e.g. "-i ~/.ssh/id")
#     --mcpu <flag>                 -mcpu tuning (default: cortex-a53, the QRB2210
#                                     Kryo core; pass "generic" for ARMv8-A baseline)
#     --backend <wayland-egl|wayland-vulkan|drm-kms-egl|software|all>
#                                   default: all
#                                     wayland-egl    GLES2 on Wayland
#                                     wayland-vulkan Vulkan on Wayland (Adreno/turnip)
#                                     drm-kms-egl    direct DRM/KMS + GBM + GLES2 (msm/DPU)
#                                     software       CPU rasterizer → DRM dumb buffer
#                                     all            build every backend in its own dir
#                                                    (drm-kms-egl is skipped when the
#                                                     synced sysroot's libdisplay-info
#                                                     < 0.2.0 — see --with-local-display-info)
#     --flutter-engine <path>       dir with lib/libflutter_engine.so + data/icudtl.dat
#                                     (bypasses auto-fetch)
#     --flutter-runtime <debug|debug-unopt|profile|release>   default: release
#     --flutter-engine-sha <sha>    pin a specific engine commit (default: pinned below)
#     --engine-url <url>            override Flutter engine tarball URL
#     --plugins-dir <path>          default: ../ivi-homescreen-plugins/plugins
#     --no-plugins                  build homescreen only
#     --with-vk-probe               also build drm_kms_vulkan_probe, the
#                                   standalone zero-copy capability probe (run
#                                   it on-target to report dma-buf scanout
#                                   support); independent of the chosen backend
#     --with-scene                  drm-kms-egl only: enable the plane compositor +
#                                     drm-cxx LayerScene present path
#                                     (BUILD_COMPOSITOR + USE_DRM_SCENE).
#     --jobs <N>                    default: nproc
#     --clean                       wipe build dir before configure
#     --prepare-only                fetch/extract toolchain, sync sysroot, engine, exit
#     --refresh-sysroot             re-rsync the rootfs from the board (needs
#                                     --device-host)
#     --toolchain-version <ver>     ARM GNU Toolchain version. Default: auto-derived
#                                     from the synced sysroot's Debian release
#                                     (bookworm→12.3.rel1, trixie→15.2.rel1). The
#                                     toolchain's bundled glibc MUST match the
#                                     target's, or libstdc++ either pulls in newer
#                                     symbols that don't resolve on-target OR hits a
#                                     pthread_cond_t layout mismatch — see build_pi.sh.
#     --toolchain-url <url>         override toolchain tarball URL
#     --toolchain-host <arch>       toolchain build-host arch (auto: x86_64/aarch64)
#     --with-local-display-info     cross-build libdisplay-info >= 0.2.0 from source
#                                     into the sysroot (static), enabling drm-kms-egl
#                                     when the board ships an older one
#     --display-info-version <ver>  libdisplay-info source tag (default 0.2.0)
#     --with-local-vulkan-headers   install newer Vulkan-Headers (header-only) into
#                                     the sysroot, enabling wayland-vulkan against an
#                                     older Vulkan SDK
#     --vulkan-headers-version <t>  Vulkan-Headers tag (default vulkan-sdk-1.4.309.0)
#
#   Deployment (post-build, over SSH to --device-host):
#     --deploy                      scp the built binary + bundle to the board and
#                                     install the kiosk systemd service. Requires
#                                     passwordless sudo on the target.
#     --bundle <path>               Flutter .desktop-homescreen bundle to install
#                                     under /opt/ivi-homescreen/bundle. If omitted,
#                                     only the engine SDK is staged.
#     --service-backend <name>      which built backend's binary to install:
#                                     wayland-egl|wayland-vulkan|drm-kms-egl|software
#                                     (default: first available, preferring
#                                     drm-kms-egl > software > wayland-egl).
#     --no-deps-install             do NOT apt-install the runtime shared libraries
#                                     on the board. Default: install them (the board
#                                     is already networked).
#     --no-mask-getty               do NOT mask getty@tty1 — keeps the login prompt.
#     --no-fullscreen               drop the -f flag from the service ExecStart.
#     --drm-mode <WxH@R>            pass --drm-mode to the service (force output mode).
#     --restart                     restart the service immediately after install
#                                     (default: install + enable, start on next boot).
#     --skip-build                  reuse existing build dirs; just deploy.
#
#     -v / --verbose
#     -h / --help
#
# Sudo on the HOST is invoked only for the qemu chroot apt steps during sysroot
# preparation. Sudo on the TARGET (over SSH) is invoked for --deploy. The rest
# runs as the invoking user.

set -euo pipefail

# ── Style helpers ────────────────────────────────────────────────────────

die()  { echo "error: $*" >&2; exit 1; }
log()  { echo "==> $*"; }
note() { [[ "${VERBOSE:-0}" -eq 1 ]] && echo "  · $*" || true; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

# ── Defaults ─────────────────────────────────────────────────────────────

DEVICE_HOST=""
SSH_PORT="22"
SSH_OPTS=""
MCPU="cortex-a53"                 # QRB2210 Kryo (Cortex-A53); "generic" = baseline
BACKEND="all"
ALL_BACKENDS=(wayland-egl wayland-vulkan drm-kms-egl software)
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
TOOLCHAIN_URL=""
WITH_LOCAL_DISPLAY_INFO=0
LIBDISPLAY_INFO_VERSION="0.2.0"   # min for drm-cxx (HDR/colorimetry EDID APIs)
LIBDISPLAY_INFO_URL=""
WITH_LOCAL_VULKAN_HEADERS=0
VULKAN_HEADERS_VERSION="vulkan-sdk-1.4.309.0"  # VK_HEADER_VERSION 309
VULKAN_HEADERS_URL=""
VERBOSE=0

# Deployment state.
DEPLOY=0
APP_BUNDLE=""
SERVICE_BACKEND=""
INSTALL_DEPS=1
MASK_GETTY=1
FULLSCREEN=1
DRM_MODE=""
RESTART=0
SKIP_BUILD=0

# ARM GNU Toolchain (host → aarch64 Linux glibc). Same triple/host-arch logic
# as build_pi.sh; version is auto-derived from the synced sysroot's Debian
# release (see derive_toolchain_version) unless --toolchain-version is given.
TC_TRIPLE="aarch64-none-linux-gnu"
case "$(uname -m)" in
    aarch64|arm64) TC_HOST="aarch64" ;;
    x86_64|amd64)  TC_HOST="x86_64" ;;
    *)             TC_HOST="x86_64" ;;
esac
TC_VERSION=""        # auto-derived unless --toolchain-version is given
declare -A TC_VERSION_FOR_RELEASE=(
    [bookworm]="12.3.rel1"   # gcc 12, glibc 2.36
    [trixie]="15.2.rel1"     # gcc 15, glibc >= 2.41
)
TC_VERSION_FALLBACK="12.3.rel1"   # used when the release can't be detected

# Pinned Flutter engine SDK from github.com/meta-flutter/flutter-engine.
ENGINE_DEFAULT_SHA="13e658725ddaa270601426d1485636157e38c34c"
ENGINE_REPO="meta-flutter/flutter-engine"
ENGINE_ARCH="arm64"  # target arch (aarch64); independent of build-host arch

# ── Argument parsing ─────────────────────────────────────────────────────

usage() {
    awk 'NR==1{next} /^[^#]/{exit} {sub(/^# ?/,""); print}' "$0"
    exit "${1:-0}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --device-host)        DEVICE_HOST="$2"; shift 2 ;;
        --ssh-port)           SSH_PORT="$2"; shift 2 ;;
        --ssh-opts)           SSH_OPTS="$2"; shift 2 ;;
        --mcpu)               MCPU="$2"; shift 2 ;;
        --backend)            BACKEND="$2"; shift 2 ;;
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
        --toolchain-url)      TOOLCHAIN_URL="$2"; shift 2 ;;
        --toolchain-version)  TC_VERSION="$2"; shift 2 ;;
        --toolchain-host)     TC_HOST="$2"; shift 2 ;;
        --with-local-display-info) WITH_LOCAL_DISPLAY_INFO=1; shift ;;
        --display-info-version)    LIBDISPLAY_INFO_VERSION="$2"; shift 2 ;;
        --with-local-vulkan-headers) WITH_LOCAL_VULKAN_HEADERS=1; shift ;;
        --vulkan-headers-version)    VULKAN_HEADERS_VERSION="$2"; shift 2 ;;
        --deploy)             DEPLOY=1; shift ;;
        --bundle)             APP_BUNDLE="$2"; shift 2 ;;
        --service-backend)    SERVICE_BACKEND="$2"; shift 2 ;;
        --no-deps-install)    INSTALL_DEPS=0; shift ;;
        --no-mask-getty)      MASK_GETTY=0; shift ;;
        --no-fullscreen)      FULLSCREEN=0; shift ;;
        --drm-mode)           DRM_MODE="$2"; shift 2 ;;
        --restart)            RESTART=1; shift ;;
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
case "$BACKEND" in wayland-egl|wayland-vulkan|drm-kms-egl|drm-kms-vulkan|software|all) ;;
    *) die "--backend must be wayland-egl|wayland-vulkan|drm-kms-egl|drm-kms-vulkan|software|all (got: $BACKEND)" ;;
esac
case "$FLUTTER_RUNTIME" in debug|debug-unopt|profile|release) ;;
    *) die "--flutter-runtime must be debug|debug-unopt|profile|release (got: $FLUTTER_RUNTIME)" ;;
esac
[[ -z "$DRM_MODE" || "$DRM_MODE" =~ ^[0-9]+x[0-9]+@[0-9]+$ ]] \
    || die "--drm-mode $DRM_MODE: expected <WxH@R> e.g. 1920x1080@120"

[[ -z "$FLUTTER_ENGINE_SHA" ]] && FLUTTER_ENGINE_SHA="$ENGINE_DEFAULT_SHA"
ENGINE_TAG="linux-engine-sdk-${FLUTTER_RUNTIME}-${ENGINE_ARCH}-${FLUTTER_ENGINE_SHA}"
[[ -z "$ENGINE_URL" ]] && \
    ENGINE_URL="https://github.com/${ENGINE_REPO}/releases/download/${ENGINE_TAG}/${ENGINE_TAG}.tar.gz"

# -mcpu tuning. "generic" => ARMv8-A baseline (no -mcpu flag).
case "$MCPU" in
    generic|"") XC_CPU_FLAGS="" ;;
    *)          XC_CPU_FLAGS="-mcpu=${MCPU}" ;;
esac

# libdisplay-info / Vulkan-Headers source archives (pinned by tag).
LIBDISPLAY_INFO_TARBALL="libdisplay-info-${LIBDISPLAY_INFO_VERSION}.tar.gz"
[[ -z "$LIBDISPLAY_INFO_URL" ]] \
    && LIBDISPLAY_INFO_URL="https://gitlab.freedesktop.org/emersion/libdisplay-info/-/archive/${LIBDISPLAY_INFO_VERSION}/${LIBDISPLAY_INFO_TARBALL}"
[[ -z "$VULKAN_HEADERS_URL" ]] \
    && VULKAN_HEADERS_URL="https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/${VULKAN_HEADERS_VERSION}.tar.gz"

# ── Resolved sandbox paths ───────────────────────────────────────────────

XC_ROOT="${IVI_XC_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/ivi-homescreen-xc-unoq}"
DOWNLOADS="$XC_ROOT/downloads"
XC_SYSROOT="$XC_ROOT/sysroot/unoq"
ENGINE_CACHE_DIR="$XC_ROOT/flutter-engine/$ENGINE_TAG"
ENGINE_SDK_DIR="$ENGINE_CACHE_DIR/engine-sdk"

# Toolchain paths depend on TC_VERSION, which may be derived from the sysroot.
# They're (re)computed in resolve_toolchain_paths once TC_VERSION is known.
TC_DIRNAME=""; TC_DIR=""; CROSS_BIN=""

build_dir_for() { echo "$REPO_DIR/cmake-build-xc-unoq-$1"; }

resolve_toolchain_paths() {
    TC_DIRNAME="arm-gnu-toolchain-${TC_VERSION}-${TC_HOST}-${TC_TRIPLE}"
    TC_DIR="$XC_ROOT/toolchain/$TC_DIRNAME"
    CROSS_BIN="$TC_DIR/bin"
    TC_TARBALL="arm-gnu-toolchain-${TC_VERSION}-${TC_HOST}-${TC_TRIPLE}.tar.xz"
    [[ -n "$TOOLCHAIN_URL" ]] \
        || TOOLCHAIN_URL="https://developer.arm.com/-/media/Files/downloads/gnu/${TC_VERSION}/binrel/${TC_TARBALL}"
    export CROSS_BIN
}

export XC_ROOT XC_SYSROOT XC_CPU_FLAGS

# SSH / rsync transport helpers. SSH_PORT + SSH_OPTS fold into one -e string.
ssh_e() { echo "ssh -p ${SSH_PORT}${SSH_OPTS:+ $SSH_OPTS}"; }
on_device() {
    # on_device <remote command...>   run a command on the UNO Q over SSH.
    # shellcheck disable=SC2086  # SSH_OPTS is intentionally word-split
    ssh -p "$SSH_PORT" $SSH_OPTS "$DEVICE_HOST" "$@"
}
on_device_tty() {
    # Like on_device but with a TTY (-t), so an interactive `sudo` on the board
    # can prompt for a password. Used for the privileged deploy step on boards
    # without passwordless sudo.
    # shellcheck disable=SC2086  # SSH_OPTS is intentionally word-split
    ssh -t -p "$SSH_PORT" $SSH_OPTS "$DEVICE_HOST" "$@"
}
scp_to_device() {
    # scp_to_device [-r] <local> <remote-path-on-board>
    # shellcheck disable=SC2086  # SSH_OPTS is intentionally word-split
    scp -P "$SSH_PORT" $SSH_OPTS "$@"
}

# ── Phase 0: preflight ───────────────────────────────────────────────────

phase0_preflight() {
    log "Phase 0: preflight"
    local missing=()
    for tool in curl xz tar rsync ssh cmake pkg-config sha256sum file; do
        command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
    done
    command -v qemu-aarch64-static >/dev/null 2>&1 \
        || command -v /usr/bin/qemu-aarch64-static >/dev/null 2>&1 \
        || missing+=("qemu-aarch64-static (package: qemu-user-static)")

    if (( ${#missing[@]} )); then
        echo "missing host tools:" >&2
        printf '  - %s\n' "${missing[@]}" >&2
        echo
        echo "  Debian/Ubuntu: sudo apt install curl xz-utils tar rsync openssh-client cmake pkg-config qemu-user-static" >&2
        echo "  Fedora:        sudo dnf install curl xz tar rsync openssh-clients cmake pkgconf-pkg-config qemu-user-static" >&2
        exit 1
    fi

    # A sysroot sync (first run / refresh) and --deploy both need the board.
    if [[ ( ! -d "$XC_SYSROOT" || "$REFRESH_SYSROOT" -eq 1 || "$DEPLOY" -eq 1 ) \
          && -z "$DEVICE_HOST" ]]; then
        die "need --device-host <user@host> to sync the sysroot from / deploy to the UNO Q"
    fi

    mkdir -p "$DOWNLOADS" "$XC_ROOT/toolchain" "$XC_ROOT/sysroot" "$XC_ROOT/flutter-engine"
    note "XC_ROOT  = $XC_ROOT"
    note "mcpu     = ${MCPU} (XC_CPU_FLAGS='${XC_CPU_FLAGS}')"
    note "backends = ${BUILD_BACKENDS[*]:-(resolved after sysroot)}"
}

# ── shared fetch / verify helpers ────────────────────────────────────────

fetch() {
    local url="$1" dest="$2"
    if [[ -s "$dest" ]]; then note "cached: $(basename "$dest")"; return; fi
    log "fetching $(basename "$dest")"
    curl --fail --location --retry 3 --retry-delay 2 \
         --continue-at - --output "$dest.part" "$url"
    mv "$dest.part" "$dest"
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

# Relativize absolute symlinks under $1 so the moved sysroot resolves correctly.
relativize_symlinks() {
    local root="$1" link target rel
    while IFS= read -r link; do
        target="$(readlink "$link")"
        rel="$(python3 -c "import os,sys; print(os.path.relpath(sys.argv[1], os.path.dirname(sys.argv[2])))" \
                "${root}${target}" "$link")"
        ln -snf "$rel" "$link"
    done < <(find "$root" -type l -lname '/*')
}

# ── Phase 1: sysroot (rsync from the live board) ─────────────────────────

# A sysroot is only "present" if it actually holds a Debian rootfs. apt-get is
# a definitive marker — required by phase 3's chroot install. This guards
# against an empty/partial directory left behind by an interrupted rsync (e.g.
# a wrong --device-host that mkdir'd the dir then failed): without it, a later
# run would treat the husk as complete and skip the sync entirely.
sysroot_populated() { [[ -e "$XC_SYSROOT/usr/bin/apt-get" ]]; }

phase1_sysroot() {
    log "Phase 1: sysroot (rsync from $DEVICE_HOST)"
    if sysroot_populated && [[ "$REFRESH_SYSROOT" -eq 0 ]]; then
        note "sysroot present; will ensure -dev packages after toolchain resolves"
        return
    fi
    # Drop any partial husk before (re)syncing so stale excludes can't linger.
    if [[ -d "$XC_SYSROOT" ]] && ! sysroot_populated; then
        log "removing incomplete sysroot from a prior interrupted sync"
        sudo rm -rf "$XC_SYSROOT"
    fi
    if [[ "$REFRESH_SYSROOT" -eq 1 && -d "$XC_SYSROOT" ]]; then
        log "removing existing sysroot for refresh"
        sudo rm -rf "$XC_SYSROOT"
    fi

    mkdir -p "$XC_SYSROOT"

    # One SSH round-trip probes the board for (a) rsync and (b) passwordless
    # sudo. Reading the whole rootfs faithfully needs root, but elevating only
    # works with PASSWORDLESS sudo: the transfer's SSH channel is
    # non-interactive, so a sudo password prompt has no TTY and dies ("a
    # terminal is required"). Without root we still get every world-readable
    # header/lib/dpkg-metadata file the cross sysroot needs; only a handful of
    # root-only files (shadow, private keys) are skipped, and they don't matter.
    local probe have_rsync have_sudo
    probe="$(on_device 'command -v rsync >/dev/null 2>&1 && echo R; sudo -n true 2>/dev/null && echo S' 2>/dev/null || true)"
    [[ "$probe" == *R* ]] && have_rsync=1 || have_rsync=0
    [[ "$probe" == *S* ]] && have_sudo=1  || have_sudo=0

    local sudo_prefix=""
    if [[ "$have_sudo" -eq 1 ]]; then
        note "passwordless sudo on board → reading rootfs as root (full fidelity)"
        sudo_prefix="sudo "
    else
        echo "  note: no passwordless sudo on $DEVICE_HOST — reading the rootfs as the" >&2
        echo "        login user. Root-only files (shadow, private keys) are skipped;" >&2
        echo "        they aren't needed for a cross sysroot. For full fidelity, enable" >&2
        echo "        NOPASSWD sudo on the board and re-run --refresh-sysroot." >&2
    fi

    # The exclude set is shared by both transports. Virtual filesystems +
    # volatile/personal dirs aren't part of a cross sysroot.
    local excl=(proc sys dev run tmp mnt media lost+found
                var/cache/apt/archives var/log home root swapfile)

    if [[ "$have_rsync" -eq 1 ]]; then
        local rc=0 rsync_excl=() rsync_path_opt=()
        local e; for e in "${excl[@]}"; do rsync_excl+=(--exclude="/$e/*"); done
        [[ -n "$sudo_prefix" ]] && rsync_path_opt=(--rsync-path="sudo rsync")
        log "rsyncing rootfs off the board (this is the slow part)"
        # No -A/-X: target xattrs/ACLs aren't needed and may be rejected by the
        # host FS. Tolerate exit 23/24 (unreadable/vanished files) — expected
        # unprivileged or against a live, changing rootfs.
        rsync -aH --numeric-ids --info=progress2 \
            -e "$(ssh_e)" \
            "${rsync_path_opt[@]}" \
            "${rsync_excl[@]}" \
            "${DEVICE_HOST}:/" "$XC_SYSROOT/" || rc=$?
        case "$rc" in
            0)      ;;
            23|24)  echo "  note: rsync partial transfer (code $rc) — skipped unreadable/vanished files; continuing." >&2 ;;
            *)      die "rsync failed (exit $rc)" ;;
        esac
    else
        # Board has no rsync (minimal image). Stream a tar over SSH instead —
        # tar is always present. --ignore-failed-read keeps going past
        # root-only files when unprivileged; --warning=no-file-changed silences
        # the noise of a live rootfs mutating mid-read.
        local tar_excl=()
        local e; for e in "${excl[@]}"; do tar_excl+=("--exclude=./$e"); done
        log "board lacks rsync — streaming rootfs via tar over SSH (this is the slow part)"
        local pstatus
        set +o pipefail
        on_device "${sudo_prefix}tar -C / --numeric-owner --ignore-failed-read --warning=no-file-changed -cf - ${tar_excl[*]} ." \
            | tar -C "$XC_SYSROOT" --numeric-owner -xf -
        pstatus=("${PIPESTATUS[@]}")
        set -o pipefail
        # Remote tar: 0 ok; 1 (files changed) / 2 (read errors) tolerated.
        case "${pstatus[0]}" in
            0|1|2) ;;
            *)     die "remote tar failed (exit ${pstatus[0]})" ;;
        esac
        [[ "${pstatus[1]}" -eq 0 ]] || die "local tar extract failed (exit ${pstatus[1]})"
    fi

    sysroot_populated \
        || die "rootfs synced but $XC_SYSROOT/usr/bin/apt-get is missing — the sync did not capture a usable Debian rootfs (check SSH access / paths)"

    sudo chown -R "$(id -u):$(id -g)" "$XC_SYSROOT" 2>/dev/null || true

    log "relativizing absolute symlinks"
    relativize_symlinks "$XC_SYSROOT"

    # glibc 2.34+ merged librt / libdl / libpthread into libc; newer Debian
    # libc6-dev drops the unversioned dev symlinks. Recreate them so
    # find_library(rt|dl|pthread) succeeds (mirrors build_pi.sh).
    log "fixing up missing libc-merged dev symlinks"
    local ma="$XC_SYSROOT/usr/lib/aarch64-linux-gnu"
    local stem sover
    for stem in rt dl pthread; do
        if [[ -d "$ma" && ! -e "$ma/lib${stem}.so" ]]; then
            sover="$(ls "$ma" | grep -E "^lib${stem}\.so\.[0-9]+$" | head -1)"
            [[ -n "$sover" ]] || { note "no lib${stem}.so.* present, skipping"; continue; }
            ln -sf "$sover" "$ma/lib${stem}.so"
            note "created $ma/lib${stem}.so -> $sover"
        fi
    done
}

# Detect the Debian codename from the synced sysroot (e.g. bookworm, trixie).
# Tries, in order: os-release VERSION_CODENAME, os-release VERSION_ID's major
# number, then /etc/debian_version. Qualcomm/Yocto-derived Debian images
# sometimes omit VERSION_CODENAME, so the numeric fallbacks matter here.
sysroot_debian_codename() {
    local osr="$XC_SYSROOT/etc/os-release"
    local code="" major=""
    if [[ -f "$osr" ]]; then
        code="$(awk -F= '/^VERSION_CODENAME=/{gsub(/"/,"",$2); print $2; exit}' "$osr")"
        [[ -n "$code" ]] && { echo "$code"; return; }
        major="$(awk -F= '/^VERSION_ID=/{gsub(/"/,"",$2); split($2,a,"."); print a[1]; exit}' "$osr")"
    fi
    if [[ -z "$major" && -f "$XC_SYSROOT/etc/debian_version" ]]; then
        major="$(cut -d. -f1 "$XC_SYSROOT/etc/debian_version" 2>/dev/null)"
    fi
    case "$major" in
        12) echo "bookworm" ;;
        13) echo "trixie" ;;
        *)  echo "" ;;
    esac
}

# Pick the ARM GNU Toolchain version to match the target's glibc, derived from
# the synced sysroot's Debian release. The toolchain's bundled glibc must match
# the target's (see header + build_pi.sh) — a mismatch breaks the build or the
# on-target run.
derive_toolchain_version() {
    [[ -n "$TC_VERSION" ]] && { note "toolchain version (explicit): $TC_VERSION"; return; }
    local codename; codename="$(sysroot_debian_codename)"
    if [[ -n "$codename" && -n "${TC_VERSION_FOR_RELEASE[$codename]:-}" ]]; then
        TC_VERSION="${TC_VERSION_FOR_RELEASE[$codename]}"
        log "detected Debian '$codename' in sysroot → toolchain $TC_VERSION"
    else
        TC_VERSION="$TC_VERSION_FALLBACK"
        echo "  WARN: could not map sysroot Debian release '${codename:-unknown}' to a" >&2
        echo "        toolchain; defaulting to $TC_VERSION. If the build fails to link or" >&2
        echo "        the binary segfaults on-target, pass --toolchain-version explicitly." >&2
    fi
}

# ── Phase 2: toolchain ───────────────────────────────────────────────────

phase2_toolchain() {
    resolve_toolchain_paths
    log "Phase 2: toolchain ($TC_DIRNAME)"
    if [[ -x "$CROSS_BIN/${TC_TRIPLE}-gcc" ]]; then
        note "toolchain present"; return
    fi
    local tarball="$DOWNLOADS/$TC_TARBALL"
    fetch "$TOOLCHAIN_URL" "$tarball"
    verify_sha256_file "$tarball" "${TOOLCHAIN_URL}.sha256"
    log "extracting toolchain"
    tar -xJf "$tarball" -C "$XC_ROOT/toolchain"
    [[ -x "$CROSS_BIN/${TC_TRIPLE}-gcc" ]] \
        || die "toolchain extracted but $CROSS_BIN/${TC_TRIPLE}-gcc not found"
}

# ── Phase 2b: Flutter engine SDK ─────────────────────────────────────────
#
# ivi-homescreen does NOT link against libflutter_engine.so — the shell
# dlopens it at runtime. The fetched SDK is a runtime artifact, staged into
# the bundle at deploy time.

phase2b_flutter_engine() {
    if [[ "$FLUTTER_ENGINE_EXPLICIT" -eq 1 ]]; then
        log "Phase 2b: Flutter engine (user-supplied)"
        [[ -f "$FLUTTER_ENGINE/lib/libflutter_engine.so" ]] \
            || die "missing $FLUTTER_ENGINE/lib/libflutter_engine.so"
        [[ -f "$FLUTTER_ENGINE/data/icudtl.dat" ]] \
            || die "missing $FLUTTER_ENGINE/data/icudtl.dat"
        return
    fi

    log "Phase 2b: Flutter engine SDK ($ENGINE_TAG)"
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
        || die "engine extracted but $ENGINE_SDK_DIR/lib/libflutter_engine.so missing"
    [[ -f "$ENGINE_SDK_DIR/data/icudtl.dat" ]] \
        || die "engine extracted but $ENGINE_SDK_DIR/data/icudtl.dat missing"
    FLUTTER_ENGINE="$ENGINE_SDK_DIR"
}

# ── Phase 3: install -dev packages into the sysroot (qemu chroot) ────────

# Mesa -dev packages carry a strict "Depends: <runtime> (= <exact version>)" on
# their runtime sibling. The UNO Q ships a VENDOR-FORKED Mesa (Qualcomm's
# *-1qcomN builds) whose runtime is already installed, so the stock Debian -dev
# can't be installed — its pinned dependency conflicts with the fork (e.g.
# "libgbm-dev depends libgbm1 (= 25.0.7-2) but 25.1.0-1qcom1 is installed").
# Worse, other -dev packages depend on it transitively (libgstreamer-plugins-
# base1.0-dev → libgbm-dev), so simply omitting it from the install list doesn't
# help — apt still pulls it in and the resolver deadlocks.
#
# The runtime fork already supplies the .so, and the GBM/GL APIs are stable
# across Mesa minor versions, so we pre-install a REPACKED copy: download the
# .deb, rewrite its control to relax the strict "(= ver)" pin on the forked
# runtime to an unversioned dependency (satisfied by the installed fork), then
# dpkg -i it. Done before the apt run, this makes the dep resolvable for
# everything downstream. Map: <dev package> → <forked runtime it pins>.
declare -A MESA_DEV_UNPIN=( [libgbm-dev]=libgbm1 )

_in_list() { local x="$1"; shift; local e; for e in "$@"; do [[ "$e" == "$x" ]] && return 0; done; return 1; }

# Run a command inside the qemu chroot (mounts must already be set up).
_chroot() { sudo chroot "$XC_SYSROOT" /usr/bin/qemu-aarch64-static "$@"; }

# Install a -dev package after relaxing its strict "(= ver)" pin on a forked
# runtime sibling. Registers it in the dpkg DB (so apt sees the dep satisfied)
# without dragging in a conflicting runtime downgrade.
sysroot_install_unpinned_dev() {
    local pkg="$1" runtime="$2"
    log "  unpinned install: $pkg (relax '$runtime (= ver)' pin — vendor-Mesa fork in place)"
    # The inner script runs in the aarch64 chroot. $pkg/$runtime are expanded
    # host-side into single-quoted assignments; everything else stays literal.
    _chroot /bin/sh -c '
        set -e
        pkg='"'$pkg'"'; runtime='"'$runtime'"'
        work=/tmp/ivi-devpkg; rm -rf "$work"; mkdir -p "$work"; cd "$work"
        apt-get download "$pkg"
        deb=$(ls -1 "${pkg}"_*.deb 2>/dev/null | head -1)
        [ -n "$deb" ] || { echo "no .deb downloaded for $pkg" >&2; exit 1; }
        dpkg-deb -R "$deb" x
        # Drop the version constraint on $runtime in the Depends field so the
        # installed vendor fork satisfies it (e.g. "libgbm1 (= 25.0.7-2)" → "libgbm1").
        sed -i -E "s/${runtime} *\([^)]*\)/${runtime}/g" x/DEBIAN/control
        dpkg-deb -b x repacked.deb
        dpkg -i repacked.deb
        cd /; rm -rf "$work"
    ' || die "failed to install $pkg (unpinned) into sysroot"
}

sysroot_apt_install() {
    # Split off the Mesa-fork -dev packages that need the unpin treatment; they
    # get pre-installed before the apt run. (They may also be dragged in
    # transitively, so this isn't just about the explicit list.)
    local strict=() unpin=() p
    for p in "$@"; do
        if [[ -n "${MESA_DEV_UNPIN[$p]:-}" ]]; then unpin+=("$p"); else strict+=("$p"); fi
    done

    local qemu_bin
    qemu_bin="$(command -v qemu-aarch64-static || echo /usr/bin/qemu-aarch64-static)"
    sudo cp "$qemu_bin" "$XC_SYSROOT/usr/bin/qemu-aarch64-static"

    sudo mount --bind /dev      "$XC_SYSROOT/dev"
    sudo mount --bind /dev/pts  "$XC_SYSROOT/dev/pts"
    sudo mount -t proc  proc    "$XC_SYSROOT/proc"
    sudo mount -t sysfs sysfs   "$XC_SYSROOT/sys"
    trap "sudo umount -lq '$XC_SYSROOT/sys' '$XC_SYSROOT/proc' '$XC_SYSROOT/dev/pts' '$XC_SYSROOT/dev' 2>/dev/null || true; sudo rm -f '$XC_SYSROOT/usr/bin/qemu-aarch64-static'" EXIT

    printf '#!/bin/sh\nexit 0\n' | sudo tee "$XC_SYSROOT/usr/sbin/update-initramfs" >/dev/null
    sudo chmod +x "$XC_SYSROOT/usr/sbin/update-initramfs"
    printf '#!/bin/sh\nexit 101\n' | sudo tee "$XC_SYSROOT/usr/sbin/policy-rc.d" >/dev/null
    sudo chmod +x "$XC_SYSROOT/usr/sbin/policy-rc.d"

    _chroot /usr/bin/apt-get -y update
    # Pre-install the unpinned Mesa-fork -dev packages so the (possibly
    # transitive) dependency is satisfied when the strict apt run resolves.
    for p in "${unpin[@]}"; do sysroot_install_unpinned_dev "$p" "${MESA_DEV_UNPIN[$p]}"; done
    (( ${#strict[@]} )) && _chroot /usr/bin/apt-get -y install --no-install-recommends "${strict[@]}"
    _chroot /usr/bin/apt-get -y clean

    sudo umount -lq "$XC_SYSROOT/sys" "$XC_SYSROOT/proc" "$XC_SYSROOT/dev/pts" "$XC_SYSROOT/dev"
    trap - EXIT
    sudo rm -f "$XC_SYSROOT/usr/bin/qemu-aarch64-static"
    sudo chown -R "$(id -u):$(id -g)" "$XC_SYSROOT"
}

sysroot_pkg_list() {
    # Shared deps across all backends. Identical Debian package names to the
    # Pi build — the UNO Q runs Debian too. On Adreno the GL/Vulkan userspace
    # is Mesa (freedreno/turnip); the -dev packages are the same generic
    # libegl-dev / libgles2-mesa-dev / libvulkan-dev names.
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
            wayland-vulkan)
                pkgs+=(libvulkan-dev mesa-vulkan-drivers) ;;
            drm-kms-egl)
                pkgs+=(libdrm-dev libgbm-dev libinput-dev libxcursor-dev libseat-dev)
                [[ "$WITH_LOCAL_DISPLAY_INFO" -eq 0 ]] \
                    && pkgs+=(libdisplay-info-dev) ;;
            software)
                pkgs+=(libdrm-dev libinput-dev) ;;
        esac
    done
    printf '%s\n' "${pkgs[@]}"
}

phase3_sysroot_devpkgs() {
    log "Phase 3: install -dev packages into sysroot (qemu chroot)"
    local pkgs=()
    mapfile -t pkgs < <(sysroot_pkg_list)
    sysroot_apt_install "${pkgs[@]}"
    relativize_symlinks "$XC_SYSROOT"
}

# ── Phase 3b/3c: optional local libdisplay-info / Vulkan-Headers ─────────

displayinfo_modversion() {
    PKG_CONFIG_LIBDIR="$XC_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$XC_SYSROOT/usr/lib/pkgconfig:$XC_SYSROOT/usr/share/pkgconfig" \
    PKG_CONFIG_SYSROOT_DIR="$XC_SYSROOT" \
        pkg-config --modversion libdisplay-info 2>/dev/null || echo 0
}
displayinfo_ge_floor() {
    [[ "$1" != "0" ]] && \
    [[ "$(printf '%s\n%s\n' "$LIBDISPLAY_INFO_VERSION" "$1" | sort -V | head -1)" == "$LIBDISPLAY_INFO_VERSION" ]]
}

phase3b_local_display_info() {
    [[ "$WITH_LOCAL_DISPLAY_INFO" -eq 1 ]] || return 0
    log "Phase 3b: local libdisplay-info (>= ${LIBDISPLAY_INFO_VERSION}, static)"

    local have; have="$(displayinfo_modversion)"
    if displayinfo_ge_floor "$have"; then
        note "sysroot already has libdisplay-info $have; skipping local build"; return
    fi
    for t in meson ninja; do
        command -v "$t" >/dev/null || die "--with-local-display-info needs '$t' on PATH"
    done

    local tarball src bld cross
    tarball="$DOWNLOADS/$LIBDISPLAY_INFO_TARBALL"
    src="$XC_ROOT/src/libdisplay-info-${LIBDISPLAY_INFO_VERSION}"
    bld="$src/build-xc-unoq"
    cross="$src/.xc-cross-unoq.ini"
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
    note "libdisplay-info $now installed (static) into unoq sysroot"
}

vulkan_header_version() {
    local h="$XC_SYSROOT/usr/include/vulkan/vulkan_core.h"
    [[ -f "$h" ]] && awk '/#define VK_HEADER_VERSION /{print $3; exit}' "$h" || echo 0
}

phase3c_local_vulkan_headers() {
    [[ "$WITH_LOCAL_VULKAN_HEADERS" -eq 1 ]] || return 0
    local want; want="$(echo "$VULKAN_HEADERS_VERSION" | awk -F. '{print $3}')"
    log "Phase 3c: local Vulkan-Headers (VK_HEADER_VERSION >= ${want})"

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
    note "Vulkan-Headers now VK_HEADER_VERSION $(vulkan_header_version) in unoq sysroot"
}

# ── Phase 4: toolchain file + pkg-config wrapper ─────────────────────────

phase4_emit_cmake() {
    local be="$1" BUILD_DIR
    BUILD_DIR="$(build_dir_for "$be")"
    log "Phase 4: emit toolchain file & pkg-config wrapper ($be)"
    mkdir -p "$BUILD_DIR"

    cat > "$BUILD_DIR/.xc-pkg-config" <<'EOF'
#!/bin/sh
# Generated by build_unoq.sh — sysroot-aware pkg-config wrapper.
export PKG_CONFIG_DIR=
export PKG_CONFIG_LIBDIR="$XC_SYSROOT/usr/lib/aarch64-linux-gnu/pkgconfig:$XC_SYSROOT/usr/lib/pkgconfig:$XC_SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$XC_SYSROOT"
exec pkg-config "$@"
EOF
    chmod +x "$BUILD_DIR/.xc-pkg-config"

    cat > "$BUILD_DIR/.xc-toolchain.cmake" <<EOF
# Generated by scripts/build_unoq.sh — do not edit.
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

# ARM GNU Toolchain targets aarch64-none-linux-gnu, but Debian splits files by
# the aarch64-linux-gnu (no -none-) multiarch triplet. Splice both in.
set(_xc_ma_inc "\${CMAKE_SYSROOT}/usr/include/aarch64-linux-gnu")
set(_xc_ma_usr "\${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu")
set(_xc_ma_lib "\${CMAKE_SYSROOT}/lib/aarch64-linux-gnu")
set(CMAKE_C_FLAGS_INIT     "-isystem \${_xc_ma_inc} -B\${_xc_ma_usr} \$ENV{XC_CPU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT   "-isystem \${_xc_ma_inc} -B\${_xc_ma_usr} \$ENV{XC_CPU_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "-L\${_xc_ma_usr} -L\${_xc_ma_lib}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-L\${_xc_ma_usr} -L\${_xc_ma_lib}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-L\${_xc_ma_usr} -L\${_xc_ma_lib}")

# CMake's compiler-ABI test ignores CMAKE_*_LINKER_FLAGS_INIT; build a static
# library for the try_compile so it doesn't fail to find crt1.o / -lm.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(PKG_CONFIG_EXECUTABLE  "$BUILD_DIR/.xc-pkg-config"
    CACHE FILEPATH "sysroot-aware pkg-config wrapper" FORCE)
EOF
    note "toolchain file: $BUILD_DIR/.xc-toolchain.cmake"
}

# ── Phase 5: configure & build ───────────────────────────────────────────

phase5_build() {
    local be="$1" BUILD_DIR
    BUILD_DIR="$(build_dir_for "$be")"
    log "Phase 5: configure & build ($be)"

    local cmake_args=(
        -S "$REPO_DIR" -B "$BUILD_DIR"
        -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/.xc-toolchain.cmake"
        -DCMAKE_BUILD_TYPE=Release
    )
    case "$be" in
        wayland-egl)
            cmake_args+=(
                -DBUILD_BACKEND_WAYLAND_EGL=ON  -DBUILD_BACKEND_WAYLAND_VULKAN=OFF
                -DBUILD_BACKEND_DRM_KMS_EGL=OFF -DBUILD_BACKEND_SOFTWARE=OFF) ;;
        wayland-vulkan)
            cmake_args+=(
                -DBUILD_BACKEND_WAYLAND_EGL=OFF -DBUILD_BACKEND_WAYLAND_VULKAN=ON
                -DBUILD_BACKEND_DRM_KMS_EGL=OFF -DBUILD_BACKEND_SOFTWARE=OFF) ;;
        drm-kms-egl)
            cmake_args+=(
                -DBUILD_BACKEND_WAYLAND_EGL=OFF -DBUILD_BACKEND_WAYLAND_VULKAN=OFF
                -DBUILD_BACKEND_DRM_KMS_EGL=ON  -DBUILD_BACKEND_SOFTWARE=OFF)
            if [[ "$WITH_SCENE" -eq 1 ]]; then
                cmake_args+=(-DBUILD_COMPOSITOR=ON -DUSE_DRM_SCENE=ON)
            fi ;;
        drm-kms-vulkan)
            cmake_args+=(
                -DBUILD_BACKEND_WAYLAND_EGL=OFF -DBUILD_BACKEND_WAYLAND_VULKAN=OFF
                -DBUILD_BACKEND_DRM_KMS_EGL=OFF -DBUILD_BACKEND_SOFTWARE=OFF
                -DBUILD_BACKEND_DRM_KMS_VULKAN=ON) ;;
        software)
            cmake_args+=(
                -DBUILD_BACKEND_WAYLAND_EGL=OFF -DBUILD_BACKEND_WAYLAND_VULKAN=OFF
                -DBUILD_BACKEND_DRM_KMS_EGL=OFF -DBUILD_BACKEND_SOFTWARE=ON) ;;
    esac

    # Standalone zero-copy capability probe, independent of the backend above.
    if [[ "$WITH_VK_PROBE" -eq 1 ]]; then
        cmake_args+=(-DBUILD_DRM_KMS_VULKAN_PROBE=ON)
    fi

    if [[ "$NO_PLUGINS" -eq 1 ]]; then
        cmake_args+=(-DDISABLE_PLUGINS=ON)
    else
        [[ -d "$PLUGINS_DIR" ]] || die "plugins dir not found: $PLUGINS_DIR (use --no-plugins or --plugins-dir)"
        cmake_args+=(-DDISABLE_PLUGINS=OFF -DPLUGINS_DIR="$PLUGINS_DIR")
    fi

    log "cmake configure"
    cmake "${cmake_args[@]}"
    log "cmake build (jobs=$JOBS)"
    cmake --build "$BUILD_DIR" -j "$JOBS"
}

# ── Phase 6: report ──────────────────────────────────────────────────────

phase6_report() {
    log "Phase 6: artifacts"
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
        if [[ -x "$BUILD_DIR/shell/drm_kms_vulkan_probe" ]]; then
            echo "    vk-probe : $BUILD_DIR/shell/drm_kms_vulkan_probe"
        fi
    done
    echo "  sysroot         : $XC_SYSROOT"
    echo "  toolchain       : $TC_DIR"
    echo "  engine (runtime): $FLUTTER_ENGINE"
    echo
    echo "Deploy to the board:"
    echo "  scripts/build_unoq.sh --device-host $DEVICE_HOST --deploy --skip-build \\"
    echo "    --bundle <your .desktop-homescreen> --service-backend ${BUILD_BACKENDS[0]}"
}

# ── Phase 7: deploy over SSH ─────────────────────────────────────────────
#
# scp the binary + bundle to the running UNO Q and install a kiosk systemd
# unit (same shape as build_pi.sh phase 7, minus the SD-card firstboot
# fixups — the board is already provisioned and networked).

resolve_service_backend() {
    local try
    if [[ -n "$SERVICE_BACKEND" ]]; then echo "$SERVICE_BACKEND"; return; fi
    for try in drm-kms-egl software wayland-egl wayland-vulkan; do
        if [[ -x "$(build_dir_for "$try")/shell/homescreen" ]]; then echo "$try"; return; fi
    done
    return 1
}

phase7_deploy() {
    log "Phase 7: deploy → $DEVICE_HOST"

    local backend home_bin
    backend="$(resolve_service_backend)" \
        || die "no built homescreen binary found; build first or pass --service-backend"
    home_bin="$(build_dir_for "$backend")/shell/homescreen"
    [[ -x "$home_bin" ]] || die "binary not found: $home_bin"
    log "installing $backend binary: $home_bin"

    local bundle_src=""
    if [[ -n "$APP_BUNDLE" ]]; then
        [[ -d "$APP_BUNDLE" ]] || die "bundle dir not found: $APP_BUNDLE"
        [[ -f "$APP_BUNDLE/lib/libflutter_engine.so" ]] \
            || die "bundle missing lib/libflutter_engine.so: $APP_BUNDLE"
        bundle_src="$APP_BUNDLE"
        log "bundle: $bundle_src"
    fi

    # Runtime shared libraries the binary dlopens/links (libdrm / libgbm /
    # libdisplay-info / gstreamer / pipewire / …). The board is networked, so
    # the install script apt-gets them. Most are already present (qcom Mesa);
    # apt-get install is a no-op for those.
    local deps_common="libwayland-client0 libwayland-cursor0 libxkbcommon0 \
libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 libpipewire-0.3-0 \
libcurl4 libsecret-1-0 libjpeg62-turbo libxml2 libglib2.0-0"
    local deps_backend=""
    case "$backend" in
        wayland-egl)    deps_backend="libegl1 libgles2 libglx-mesa0 libgl1-mesa-dri" ;;
        wayland-vulkan) deps_backend="libvulkan1 mesa-vulkan-drivers" ;;
        drm-kms-egl)    deps_backend="libdrm2 libgbm1 libinput10 libdisplay-info2 \
libxcursor1 dmz-cursor-theme libegl1 libgles2 libgl1-mesa-dri" ;;
        software)       deps_backend="libdrm2 libinput10" ;;
    esac

    # Kiosk ExecStart flags: -f forces fullscreen at the panel's native mode
    # (without it a smaller configured size letterboxes); --drm-mode overrides
    # the EDID-preferred output mode.
    local exec_extra=""
    [[ "$FULLSCREEN" -eq 1 ]] && exec_extra="${exec_extra} -f"
    [[ -n "$DRM_MODE" ]]      && exec_extra="${exec_extra} --drm-mode ${DRM_MODE}"

    # Deploy strategy: the board has no passwordless sudo, and the transfer
    # channel can't carry an interactive prompt. So stage everything to a temp
    # dir as the login user (no sudo), then run ONE privileged install script
    # over `ssh -t` — sudo prompts once on the allocated TTY and does all the
    # root work (apt, install into /usr/local + /opt, systemd unit, enable).
    local stage="/tmp/ivi-deploy.$$"
    local local_install; local_install="$(mktemp)"

    # Host-injected values go in the header (printf %q keeps them safe); the
    # body is literal so $STAGE / $(dirname) / the unit heredoc's ${EXEC_EXTRA}
    # all resolve on the BOARD at run time, not here.
    {
        echo '#!/bin/bash'
        echo 'set -e'
        printf 'DO_DEPS=%q\n'    "$INSTALL_DEPS"
        printf 'DEPS=%q\n'       "$deps_common $deps_backend"
        printf 'EXEC_EXTRA=%q\n' "$exec_extra"
        printf 'DO_BUNDLE=%q\n'  "$([[ -n "$bundle_src" ]] && echo 1 || echo 0)"
        printf 'MASK_GETTY=%q\n' "$MASK_GETTY"
        printf 'DO_RESTART=%q\n' "$RESTART"
    } > "$local_install"
    cat >> "$local_install" <<'BODY'
STAGE="$(cd "$(dirname "$0")" && pwd)"

if [ "$DO_DEPS" = 1 ]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    # shellcheck disable=SC2086
    apt-get install -y --no-install-recommends $DEPS
fi

install -m0755 "$STAGE/homescreen" /usr/local/bin/ivi-homescreen

if [ "$DO_BUNDLE" = 1 ]; then
    rm -rf /opt/ivi-homescreen/bundle
    mkdir -p /opt/ivi-homescreen/bundle
    cp -a "$STAGE/bundle/." /opt/ivi-homescreen/bundle/
else
    mkdir -p /opt/ivi-homescreen/bundle/lib /opt/ivi-homescreen/bundle/data
    install -m0644 "$STAGE/engine/lib/libflutter_engine.so" /opt/ivi-homescreen/bundle/lib/
    install -m0644 "$STAGE/engine/data/icudtl.dat" /opt/ivi-homescreen/bundle/data/
fi

cat > /etc/systemd/system/ivi-homescreen.service <<UNIT
[Unit]
Description=ivi-homescreen Flutter shell (kiosk)
DefaultDependencies=no
Wants=systemd-udev-settle.service
After=systemd-udev-settle.service local-fs.target
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
ExecStart=/usr/local/bin/ivi-homescreen${EXEC_EXTRA} -b /opt/ivi-homescreen/bundle
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
UNIT

systemctl daemon-reload
systemctl enable ivi-homescreen.service
[ "$MASK_GETTY" = 1 ]  && systemctl mask getty@tty1.service || true
[ "$DO_RESTART" = 1 ]  && { systemctl restart ivi-homescreen.service || true; }
rm -rf "$STAGE"
echo "install.sh: done"
BODY

    log "staging payload → $DEVICE_HOST:$stage"
    on_device "mkdir -p '$stage'"
    scp_to_device "$home_bin"       "${DEVICE_HOST}:${stage}/homescreen"  >/dev/null
    scp_to_device "$local_install"  "${DEVICE_HOST}:${stage}/install.sh"  >/dev/null
    if [[ -n "$bundle_src" ]]; then
        log "staging bundle ($bundle_src)"
        scp_to_device -r "$bundle_src" "${DEVICE_HOST}:${stage}/bundle"   >/dev/null
    else
        log "no --bundle given; staging engine SDK only"
        on_device "mkdir -p '$stage/engine/lib' '$stage/engine/data'"
        scp_to_device "$FLUTTER_ENGINE/lib/libflutter_engine.so" "${DEVICE_HOST}:${stage}/engine/lib/" >/dev/null
        scp_to_device "$FLUTTER_ENGINE/data/icudtl.dat"          "${DEVICE_HOST}:${stage}/engine/data/" >/dev/null
    fi
    rm -f "$local_install"

    log "running privileged install on the board (sudo will prompt once)"
    on_device_tty "chmod +x '$stage/install.sh' && sudo bash '$stage/install.sh'" \
        || die "remote install failed; payload left at $DEVICE_HOST:$stage for inspection"

    log "deploy complete"
    echo
    echo "  The kiosk service is installed + enabled on $DEVICE_HOST."
    [[ "$RESTART" -eq 1 ]] || echo "  It starts on next boot (pass --restart to start it now)."
    echo
    echo "  Note: the UNO Q's stock Debian may already run a display server"
    echo "  (Weston / Arduino App Lab UI) holding DRM master. For the"
    echo "  drm-kms-egl backend, stop/disable that first or it will fight"
    echo "  for the framebuffer."
    echo
    echo "  Inspect the service on the target:"
    echo "    sudo systemctl status ivi-homescreen.service"
    echo "    sudo journalctl -u ivi-homescreen.service -b"
}

# ── Main ─────────────────────────────────────────────────────────────────

# Backends are resolved AFTER the sysroot sync, because the drm-kms-egl gate
# depends on the synced sysroot's libdisplay-info version.
resolve_backends() {
    if [[ "$BACKEND" == "all" ]]; then
        BUILD_BACKENDS=("${ALL_BACKENDS[@]}")
        if [[ "$WITH_LOCAL_DISPLAY_INFO" -eq 0 ]]; then
            local have; have="$(displayinfo_modversion)"
            if ! displayinfo_ge_floor "$have"; then
                log "note: sysroot libdisplay-info '$have' < ${LIBDISPLAY_INFO_VERSION}; skipping drm-kms-egl (pass --with-local-display-info to include it)"
                BUILD_BACKENDS=()
                local be
                for be in "${ALL_BACKENDS[@]}"; do
                    [[ "$be" == "drm-kms-egl" ]] && continue
                    BUILD_BACKENDS+=("$be")
                done
            fi
        fi
    else
        BUILD_BACKENDS=("$BACKEND")
        if [[ "$BACKEND" == "drm-kms-egl" && "$WITH_LOCAL_DISPLAY_INFO" -eq 0 ]]; then
            local have; have="$(displayinfo_modversion)"
            displayinfo_ge_floor "$have" \
                || die "drm-kms-egl needs libdisplay-info >= ${LIBDISPLAY_INFO_VERSION} (sysroot has '$have'); pass --with-local-display-info to cross-build it"
        fi
    fi
    export BUILD_BACKENDS
}

# BUILD_BACKENDS is referenced in phase0/sysroot_pkg_list before the full
# resolution; seed it so set -u is happy, then refine after the sysroot exists.
BUILD_BACKENDS=("${ALL_BACKENDS[@]}")
[[ "$BACKEND" != "all" ]] && BUILD_BACKENDS=("$BACKEND")

phase0_preflight
phase1_sysroot
derive_toolchain_version
phase2_toolchain
phase2b_flutter_engine
phase3_sysroot_devpkgs
phase3b_local_display_info
phase3c_local_vulkan_headers

# Now that libdisplay-info's final version is known, finalize the backend set.
resolve_backends

if [[ "$PREPARE_ONLY" -eq 1 ]]; then
    log "prepare-only: stopping before configure"
    echo "  toolchain: $TC_DIR"
    echo "  sysroot  : $XC_SYSROOT"
    echo "  engine   : $FLUTTER_ENGINE"
    echo "  backends : ${BUILD_BACKENDS[*]}"
    exit 0
fi

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    for be in "${BUILD_BACKENDS[@]}"; do
        if [[ "$CLEAN" -eq 1 ]]; then
            log "wiping $(build_dir_for "$be")"
            rm -rf "$(build_dir_for "$be")"
        fi
        phase4_emit_cmake "$be"
        phase5_build "$be"
    done
else
    log "--skip-build: reusing existing build dirs"
fi

phase6_report

if [[ "$DEPLOY" -eq 1 ]]; then
    phase7_deploy
fi