#!/usr/bin/env bash
# Copyright 2026 Toyota Connected North America
#
# Cross-build of ivi-homescreen (drm_kms_egl backend) for the Boundary Devices
# Nitrogen8MM (NXP i.MX8MM — quad Cortex-A53, Vivante GC7000NanoUltra GPU,
# imx-drm/LCDIF display, Yocto "fsl-imx-wayland" userspace, GLES2/EGL only).
#
# Unlike build_unoq.sh / build_radxa_zero3.sh, this board's userspace comes from
# a local Yocto build, not a Debian image — so there is no rootfs to rsync and
# no apt to install -dev packages. Instead this script reuses the Yocto build
# tree directly:
#
#   * the weston recipe's recipe-sysroot is already a complete cross dev sysroot
#     (libdrm/gbm/EGL/GLESv2/libinput/libudev/libxkbcommon/libseat + headers),
#     and its recipe-sysroot-native ships the matching aarch64-poky-linux gcc;
#   * libdisplay-info >= 0.2.0 (required by drm-cxx, absent from the image) is
#     cross-built static and installed into that sysroot;
#   * the Flutter engine is dlopen'd at runtime, so it is NOT needed to build —
#     a self-contained app bundle (lib/libflutter_engine.so + lib/libapp.so +
#     data/) is deployed instead.
#
# The i.MX8MM LCDIF has a single primary plane (no HW cursor plane) and the
# image has no libxcursor, so the build uses the GL-composited cursor fallback.
#
# Sandbox layout (nothing is written under /usr, /opt, etc. on the host):
#
#   $XC_ROOT/                  default: $XDG_CACHE_HOME/ivi-homescreen-xc-nitrogen8mm
#     downloads/               cached libdisplay-info tarball
#     src/                     extracted libdisplay-info source + meson build
#
#   <repo>/cmake-build-xc-nitrogen8mm-drm-kms-egl/   CMake build dir (gitignored)
#
# NOTE: libdisplay-info is installed (static) INTO the weston recipe-sysroot.
# That is a build artifact you own; re-running weston's do_populate_sysroot in
# Yocto would wipe it, in which case just re-run this script (the step is
# idempotent).
#
# Host requirements: Linux with cmake, ninja, meson, pkg-config, curl, tar,
# ssh/scp, file. SSH key access to the board (root@nitrogen8mm.local).
#
# Usage:
#   scripts/build_nitrogen8mm.sh [options]
#     --yocto-build <dir>     Yocto build dir holding tmp/work/...
#                               (default: /mnt/raid10/yocto/nxp-imx/bld-wayland)
#     --machine-tuple <t>     Yocto package arch dir under tmp/work
#                               (default: armv8a-mx8mm-poky-linux)
#     --device-host <u@host>  SSH target for --deploy (default: root@nitrogen8mm.local)
#     --ssh-port <n>          SSH port (default: 22)
#     --ssh-opts <str>        extra ssh/scp options (e.g. "-i ~/.ssh/id_board")
#     --bundle <path>         self-contained Flutter app bundle to deploy
#                               (default: ../flutter-wonderous-app/.desktop-homescreen)
#     --plugins-dir <path>    ivi-homescreen-plugins/plugins (default: ../ivi-homescreen-plugins/plugins)
#     --no-plugins            build homescreen only
#     --with-scene            enable the plane compositor + drm-cxx LayerScene
#                               (BUILD_COMPOSITOR)
#     --display-info-version <v>  libdisplay-info source tag (default: 0.2.0)
#     --jobs <N>              parallel build jobs (default: nproc)
#     --clean                 wipe the CMake build dir before configuring
#     --prepare-only          locate toolchain + build libdisplay-info, then exit
#     --deploy                scp binary + bundle to the board and install the
#                               seatd + kiosk systemd units
#     --restart               with --deploy, (re)start the service now
#     -h | --help             this help
set -euo pipefail

# ── Defaults ─────────────────────────────────────────────────────────────
YOCTO_BUILD="/mnt/raid10/yocto/nxp-imx/bld-wayland"
MACHINE_TUPLE="armv8a-mx8mm-poky-linux"
DEVICE_HOST="root@nitrogen8mm.local"
SSH_PORT="22"
SSH_OPTS=""
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUNDLE="$REPO_DIR/../flutter-wonderous-app/.desktop-homescreen"
PLUGINS_DIR="$REPO_DIR/../ivi-homescreen-plugins/plugins"
NO_PLUGINS=0
WITH_SCENE=0
LIBDISPLAY_INFO_VERSION="0.2.0"
JOBS="$(nproc)"
CLEAN=0
PREPARE_ONLY=0
DO_DEPLOY=0
DO_RESTART=0

XC_ROOT="${XC_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/ivi-homescreen-xc-nitrogen8mm}"
BUILD_DIR="$REPO_DIR/cmake-build-xc-nitrogen8mm-drm-kms-egl"

# i.MX8MM Cortex-A53 tune, mirrored from the weston recipe's CC.
XC_CPU_FLAGS="-march=armv8-a+crc+crypto -mbranch-protection=standard"
CROSS_TRIPLE="aarch64-poky-linux"

# ── Helpers ──────────────────────────────────────────────────────────────
log()  { printf '\033[1;34m[build]\033[0m %s\n' "$*"; }
note() { printf '       %s\n' "$*"; }
die()  { printf '\033[1;31m[build] error:\033[0m %s\n' "$*" >&2; exit 1; }

ssh_dev() { ssh -p "$SSH_PORT" $SSH_OPTS "$DEVICE_HOST" "$@"; }
scp_dev() { scp -P "$SSH_PORT" $SSH_OPTS "$@"; }

# ── Arg parsing ──────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --yocto-build)         YOCTO_BUILD="$2"; shift 2 ;;
    --machine-tuple)       MACHINE_TUPLE="$2"; shift 2 ;;
    --device-host)         DEVICE_HOST="$2"; shift 2 ;;
    --ssh-port)            SSH_PORT="$2"; shift 2 ;;
    --ssh-opts)            SSH_OPTS="$2"; shift 2 ;;
    --bundle)              BUNDLE="$2"; shift 2 ;;
    --plugins-dir)         PLUGINS_DIR="$2"; shift 2 ;;
    --no-plugins)          NO_PLUGINS=1; shift ;;
    --with-scene)          WITH_SCENE=1; shift ;;
    --display-info-version) LIBDISPLAY_INFO_VERSION="$2"; shift 2 ;;
    --jobs)                JOBS="$2"; shift 2 ;;
    --clean)               CLEAN=1; shift ;;
    --prepare-only)        PREPARE_ONLY=1; shift ;;
    --deploy)              DO_DEPLOY=1; shift ;;
    --restart)             DO_RESTART=1; shift ;;
    -h|--help)             sed -n '2,/^set -euo/p' "$0" | sed 's/^# \{0,1\}//; s/^#$//'; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

LIBDISPLAY_INFO_URL="https://gitlab.freedesktop.org/emersion/libdisplay-info/-/archive/${LIBDISPLAY_INFO_VERSION}/libdisplay-info-${LIBDISPLAY_INFO_VERSION}.tar.gz"

# ── Phase 0: preflight ───────────────────────────────────────────────────
phase0_preflight() {
  log "Phase 0: preflight"
  local missing=()
  for t in cmake ninja meson pkg-config curl tar file; do
    command -v "$t" >/dev/null || missing+=("$t")
  done
  if [[ "$DO_DEPLOY" -eq 1 ]]; then
    for t in ssh scp; do command -v "$t" >/dev/null || missing+=("$t"); done
  fi
  if [[ ${#missing[@]} -gt 0 ]]; then
    die "missing host tools: ${missing[*]}
  Fedora: sudo dnf install cmake ninja-build meson pkgconf-pkg-config curl tar file openssh-clients
  Debian: sudo apt install cmake ninja-build meson pkg-config curl tar file openssh-client"
  fi
  [[ -d "$REPO_DIR/third_party/drm-cxx" && -f "$REPO_DIR/third_party/drm-cxx/CMakeLists.txt" ]] \
    || die "third_party/drm-cxx missing — run: git submodule update --init --recursive third_party/drm-cxx"
}

# ── Phase 1: locate Yocto sysroot + cross toolchain ──────────────────────
phase1_locate() {
  log "Phase 1: locate Yocto weston sysroot + cross toolchain"
  local weston_glob="$YOCTO_BUILD/tmp/work/$MACHINE_TUPLE/weston"
  [[ -d "$weston_glob" ]] \
    || die "weston not built under $weston_glob
  Point --yocto-build at your i.MX8MM Yocto build (the dir with tmp/work/...),
  and ensure weston has been built (it stages the full graphics dev sysroot)."

  # Newest weston version dir (|| true: don't let an empty glob trip set -e).
  local wdir
  wdir="$( (ls -d "$weston_glob"/*/ 2>/dev/null || true) | sort -V | tail -1)"
  [[ -n "$wdir" ]] || die "no weston version dir under $weston_glob"

  XC_SYSROOT="${wdir%/}/recipe-sysroot"
  local native="${wdir%/}/recipe-sysroot-native"
  CROSS_BIN="$native/usr/bin/$CROSS_TRIPLE"

  [[ -x "$CROSS_BIN/$CROSS_TRIPLE-gcc" ]] \
    || die "cross gcc not found: $CROSS_BIN/$CROSS_TRIPLE-gcc"
  [[ -f "$XC_SYSROOT/usr/include/EGL/egl.h" ]] \
    || die "sysroot missing EGL headers: $XC_SYSROOT (is this the weston recipe-sysroot?)"

  export XC_SYSROOT CROSS_BIN XC_CPU_FLAGS
  note "sysroot   = $XC_SYSROOT"
  note "cross gcc = $CROSS_BIN/$CROSS_TRIPLE-gcc ($("$CROSS_BIN/$CROSS_TRIPLE-gcc" -dumpversion 2>/dev/null))"
  note "cpu flags = $XC_CPU_FLAGS"
}

# ── Phase 2: libdisplay-info >= 0.2.0 (static) into the sysroot ───────────
displayinfo_ok() {
  PKG_CONFIG_LIBDIR="$XC_SYSROOT/usr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$XC_SYSROOT" \
    pkg-config --atleast-version="$LIBDISPLAY_INFO_VERSION" libdisplay-info 2>/dev/null
}

phase2_display_info() {
  log "Phase 2: libdisplay-info >= $LIBDISPLAY_INFO_VERSION (static, drm-cxx dep)"
  if displayinfo_ok; then
    note "sysroot already has libdisplay-info $(PKG_CONFIG_LIBDIR="$XC_SYSROOT/usr/lib/pkgconfig" PKG_CONFIG_SYSROOT_DIR="$XC_SYSROOT" pkg-config --modversion libdisplay-info); skipping"
    return
  fi

  mkdir -p "$XC_ROOT/downloads" "$XC_ROOT/src"
  local tarball="$XC_ROOT/downloads/libdisplay-info-${LIBDISPLAY_INFO_VERSION}.tar.gz"
  local src="$XC_ROOT/src/libdisplay-info-${LIBDISPLAY_INFO_VERSION}"
  local cross="$XC_ROOT/src/di-cross.ini"
  local bld="$XC_ROOT/src/di-build"

  if [[ ! -f "$tarball" ]]; then
    log "fetching libdisplay-info $LIBDISPLAY_INFO_VERSION"
    curl -fsSL -o "$tarball" "$LIBDISPLAY_INFO_URL" || die "download failed: $LIBDISPLAY_INFO_URL"
  fi
  if [[ ! -f "$src/meson.build" ]]; then
    rm -rf "$src"; local tmp; tmp="$(mktemp -d "$XC_ROOT/src/.unpack.XXXXXX")"
    tar -xzf "$tarball" -C "$tmp"; mv "$tmp"/*/ "$src"; rmdir "$tmp"
  fi

  cat > "$cross" <<EOF
[binaries]
c = '$CROSS_BIN/$CROSS_TRIPLE-gcc'
cpp = '$CROSS_BIN/$CROSS_TRIPLE-g++'
ar = '$CROSS_BIN/$CROSS_TRIPLE-ar'
strip = '$CROSS_BIN/$CROSS_TRIPLE-strip'
pkg-config = 'pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[built-in options]
c_args = ['--sysroot=$XC_SYSROOT', '-march=armv8-a+crc+crypto', '-mbranch-protection=standard']
c_link_args = ['--sysroot=$XC_SYSROOT']
EOF

  log "configuring + building libdisplay-info (meson cross, static)"
  rm -rf "$bld"
  PKG_CONFIG_LIBDIR="$XC_SYSROOT/usr/lib/pkgconfig:$XC_SYSROOT/usr/share/pkgconfig" \
  PKG_CONFIG_SYSROOT_DIR="$XC_SYSROOT" \
    meson setup "$bld" "$src" --cross-file "$cross" \
      --prefix /usr --libdir lib --buildtype release --default-library static
  ninja -C "$bld"
  DESTDIR="$XC_SYSROOT" ninja -C "$bld" install
  # Drop the shared sibling (install builds only static, but be safe) so the
  # linker picks the .a and homescreen needs no libdisplay-info.so on-target.
  rm -f "$XC_SYSROOT/usr/lib/libdisplay-info.so"*

  displayinfo_ok || die "libdisplay-info install did not yield >= $LIBDISPLAY_INFO_VERSION"
  note "installed libdisplay-info (static) into $XC_SYSROOT"
}

# ── Phase 3: emit toolchain file + sysroot-aware pkg-config wrapper ───────
phase3_emit() {
  log "Phase 3: emit toolchain file + pkg-config wrapper"
  mkdir -p "$BUILD_DIR"

  cat > "$BUILD_DIR/.xc-pkg-config" <<'EOF'
#!/bin/sh
# Sysroot-aware pkg-config wrapper for the Yocto (weston recipe) sysroot.
export PKG_CONFIG_DIR=
export PKG_CONFIG_LIBDIR="$XC_SYSROOT/usr/lib/pkgconfig:$XC_SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR="$XC_SYSROOT"
unset PKG_CONFIG_PATH
exec pkg-config "$@"
EOF
  chmod +x "$BUILD_DIR/.xc-pkg-config"

  cat > "$BUILD_DIR/.xc-toolchain.cmake" <<EOF
# Generated by scripts/build_nitrogen8mm.sh — do not edit.
# i.MX8MM, Yocto weston recipe-sysroot + aarch64-poky-linux gcc.
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT          "\$ENV{XC_SYSROOT}")
set(CMAKE_C_COMPILER       "\$ENV{CROSS_BIN}/$CROSS_TRIPLE-gcc")
set(CMAKE_CXX_COMPILER     "\$ENV{CROSS_BIN}/$CROSS_TRIPLE-g++")
set(CMAKE_FIND_ROOT_PATH   "\$ENV{XC_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Yocto sysroot has no Debian multiarch layout (libs live in /usr/lib).
set(_xc_cpu "-march=armv8-a+crc+crypto -mbranch-protection=standard")
set(CMAKE_C_FLAGS_INIT   "\${_xc_cpu}")
set(CMAKE_CXX_FLAGS_INIT "\${_xc_cpu}")

set(PKG_CONFIG_EXECUTABLE  "$BUILD_DIR/.xc-pkg-config"
    CACHE FILEPATH "sysroot-aware pkg-config wrapper" FORCE)
EOF
  note "toolchain file: $BUILD_DIR/.xc-toolchain.cmake"
}

# ── Phase 4: configure & build ───────────────────────────────────────────
phase4_build() {
  log "Phase 4: configure & build (drm-kms-egl, jobs=$JOBS)"
  [[ "$CLEAN" -eq 1 ]] && rm -rf "$BUILD_DIR"/CMakeCache.txt "$BUILD_DIR"/CMakeFiles
  export XC_SYSROOT CROSS_BIN
  export PATH="$CROSS_BIN:$PATH"

  local args=(
    -G Ninja -S "$REPO_DIR" -B "$BUILD_DIR"
    -DCMAKE_TOOLCHAIN_FILE="$BUILD_DIR/.xc-toolchain.cmake"
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_BACKEND_WAYLAND_EGL=OFF -DBUILD_BACKEND_WAYLAND_VULKAN=OFF
    -DBUILD_BACKEND_DRM_KMS_EGL=ON  -DBUILD_BACKEND_SOFTWARE=OFF
  )
  [[ "$WITH_SCENE" -eq 1 ]] && args+=(-DBUILD_COMPOSITOR=ON)
  if [[ "$NO_PLUGINS" -eq 1 ]]; then
    args+=(-DDISABLE_PLUGINS=ON)
  else
    [[ -d "$PLUGINS_DIR" ]] || die "plugins dir not found: $PLUGINS_DIR (use --no-plugins or --plugins-dir)"
    args+=(-DDISABLE_PLUGINS=OFF -DPLUGINS_DIR="$PLUGINS_DIR")
  fi

  cmake "${args[@]}"
  cmake --build "$BUILD_DIR" -j "$JOBS"
}

# ── Phase 5: report ──────────────────────────────────────────────────────
phase5_report() {
  local exe="$BUILD_DIR/shell/homescreen"
  [[ -x "$exe" ]] || die "no homescreen binary produced"
  log "Phase 5: artifacts"
  echo "  binary  : $exe"
  file "$exe" | sed 's/^/      /'
  echo "  sysroot : $XC_SYSROOT"
}

# ── Phase 6: deploy ──────────────────────────────────────────────────────
phase6_deploy() {
  [[ "$DO_DEPLOY" -eq 1 ]] || return 0
  log "Phase 6: deploy to $DEVICE_HOST"
  local exe="$BUILD_DIR/shell/homescreen"
  [[ -d "$BUNDLE" && -f "$BUNDLE/lib/libflutter_engine.so" ]] \
    || die "bundle not found / not self-contained: $BUNDLE
  Expected a dir with lib/libflutter_engine.so + lib/libapp.so + data/."

  local stage="/tmp/ivi-homescreen-stage"
  ssh_dev "rm -rf '$stage' && mkdir -p '$stage'"
  log "staging binary + bundle"
  scp_dev "$exe" "$DEVICE_HOST:$stage/homescreen" >/dev/null
  scp_dev -r "$BUNDLE" "$DEVICE_HOST:$stage/bundle" >/dev/null

  log "installing on board (seatd + kiosk units)"
  ssh_dev "bash -s" <<'REMOTE'
set -e
stage="/tmp/ivi-homescreen-stage"
install -Dm755 "$stage/homescreen" /usr/local/bin/ivi-homescreen
rm -rf /opt/ivi-homescreen/bundle
mkdir -p /opt/ivi-homescreen
cp -a "$stage/bundle" /opt/ivi-homescreen/bundle

# seatd: the image ships the binary but runs no seatd; libseat's logind backend
# has no seat in a headless systemd service, so drive a VT-bound seatd seat.
cat > /etc/systemd/system/seatd.service <<'UNIT'
[Unit]
Description=Seat management daemon
[Service]
Type=simple
ExecStart=/usr/bin/seatd -g root
Restart=on-failure
[Install]
WantedBy=multi-user.target
UNIT

cat > /etc/systemd/system/ivi-homescreen.service <<'UNIT'
[Unit]
Description=ivi-homescreen (drm_kms_egl)
After=seatd.service systemd-udev-settle.service
Wants=seatd.service
[Service]
Type=simple
Environment=LIBSEAT_BACKEND=seatd
Environment=XDG_RUNTIME_DIR=/run/user/0
# Flutter apps (path_provider/shared_preferences) require HOME; a systemd
# service has none by default, unlike an interactive login shell.
Environment=HOME=/root
WorkingDirectory=/opt/ivi-homescreen
ExecStartPre=/bin/mkdir -p /run/user/0
ExecStartPre=-/bin/sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g" 2>/dev/null || true; done'
ExecStart=/usr/local/bin/ivi-homescreen -b /opt/ivi-homescreen/bundle
Restart=on-failure
RestartSec=3
[Install]
WantedBy=multi-user.target
UNIT

systemctl daemon-reload
systemctl enable seatd.service ivi-homescreen.service
rm -rf "$stage"
echo "install: done"
REMOTE

  if [[ "$DO_RESTART" -eq 1 ]]; then
    log "starting service"
    ssh_dev "systemctl restart seatd.service ivi-homescreen.service || true"
  fi
  echo
  echo "  Deployed + enabled on $DEVICE_HOST (starts on boot)."
  echo "  Inspect: ssh $DEVICE_HOST 'journalctl -u ivi-homescreen -b'"
  [[ "$DO_RESTART" -eq 1 ]] || echo "  Pass --restart to start it now."
}

# ── Main ─────────────────────────────────────────────────────────────────
phase0_preflight
phase1_locate
phase2_display_info
if [[ "$PREPARE_ONLY" -eq 1 ]]; then
  log "prepare-only: toolchain located + libdisplay-info ready; exiting"
  exit 0
fi
phase3_emit
phase4_build
phase5_report
phase6_deploy
log "done"
