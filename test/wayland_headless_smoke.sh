#!/usr/bin/env bash
#
# Copyright 2026 Toyota Connected North America
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# Runtime smoke for the Wayland backends: does a compositor-enabled build put
# pixels on a screen.
#
#   1. sway runs headless with the pixman renderer, so no GPU is needed.
#   2. homescreen renders through Mesa's software rasterizer.
#   3. grim captures the output and the frame is checked for content.
#
# The check is deliberately weak -- "not a single flat color" -- because that
# is what a backend defect looks like from outside: every failure this is meant
# to catch (a composite that never lands, a surface that never maps, an engine
# that never presents) ends in a uniform frame. A build job cannot see any of
# them; they are all runtime.
#
# Mesa's driver choice on the Wayland platform comes from the compositor's
# device, so LIBGL_ALWAYS_SOFTWARE and GALLIUM_DRIVER do nothing here;
# MESA_LOADER_DRIVER_OVERRIDE is the knob that works.
#
# Environment:
#   HOMESCREEN   path to the homescreen binary (required)
#   BUNDLE       path to the Flutter bundle directory (required)
#   SECONDS_RUN  seconds to run before capturing (default 25)
#   OUT_DIR      where logs and captures land (default ./wayland-smoke-out)
#
# Usage:
#   env HOMESCREEN=build/shell/homescreen BUNDLE=/path/to/bundle \
#       test/wayland_headless_smoke.sh
#
#   # prerequisite check only, no launch:
#   test/wayland_headless_smoke.sh --check
#
set -euo pipefail

HOMESCREEN="${HOMESCREEN:-}"
BUNDLE="${BUNDLE:-}"
SECONDS_RUN="${SECONDS_RUN:-25}"
OUT_DIR="${OUT_DIR:-$PWD/wayland-smoke-out}"
CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1

missing=0
need() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "missing: $1"
    missing=1
  fi
}
need sway
need grim
need python3

if [ "$missing" -ne 0 ]; then
  echo "prerequisites not met"
  exit 1
fi
echo "prerequisites ok: sway, grim, python3"

if [ "$CHECK_ONLY" -eq 1 ]; then
  exit 0
fi

[ -n "$HOMESCREEN" ] || { echo "HOMESCREEN is required"; exit 1; }
[ -n "$BUNDLE" ] || { echo "BUNDLE is required"; exit 1; }
[ -x "$HOMESCREEN" ] || { echo "not executable: $HOMESCREEN"; exit 1; }
[ -d "$BUNDLE" ] || { echo "not a directory: $BUNDLE"; exit 1; }

mkdir -p "$OUT_DIR"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
mkdir -p "$XDG_RUNTIME_DIR"

# sway 1.11 does not log the display name it picked, so take it from the
# sockets that appear. Unset WAYLAND_DISPLAY first or sway nests instead of
# creating one of its own.
ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | sort > "$OUT_DIR/sockets.before" || true
unset WAYLAND_DISPLAY || true

SWAY_CFG="$OUT_DIR/sway.conf"
echo "exec_always true" > "$SWAY_CFG"
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1 \
  WLR_RENDERER=pixman \
  sway -c "$SWAY_CFG" > "$OUT_DIR/sway.log" 2>&1 &
SWAY_PID=$!

cleanup() {
  kill "$SWAY_PID" 2>/dev/null || true
  wait "$SWAY_PID" 2>/dev/null || true
}
trap cleanup EXIT

DISPLAY_NAME=""
for _ in $(seq 1 60); do
  ls "$XDG_RUNTIME_DIR"/wayland-* 2>/dev/null | sort > "$OUT_DIR/sockets.after" || true
  DISPLAY_NAME="$(comm -13 "$OUT_DIR/sockets.before" "$OUT_DIR/sockets.after" \
    | grep -v '\.lock$' | head -1 || true)"
  [ -n "$DISPLAY_NAME" ] && break
  sleep 0.5
done
if [ -z "$DISPLAY_NAME" ]; then
  echo "sway did not create a socket; see $OUT_DIR/sway.log"
  exit 1
fi
DISPLAY_NAME="$(basename "$DISPLAY_NAME")"
echo "sway display: $DISPLAY_NAME"

WAYLAND_DISPLAY="$DISPLAY_NAME" MESA_LOADER_DRIVER_OVERRIDE=llvmpipe \
  timeout -k 5 "$SECONDS_RUN" "$HOMESCREEN" -b "$BUNDLE" \
    --backend=wayland-egl -w 1280 --height 720 \
    > "$OUT_DIR/homescreen.log" 2>&1 &
SHELL_PID=$!

sleep $(( SECONDS_RUN > 10 ? SECONDS_RUN - 8 : 2 ))
SHOT="$OUT_DIR/frame.png"
if ! WAYLAND_DISPLAY="$DISPLAY_NAME" grim "$SHOT" 2>"$OUT_DIR/grim.log"; then
  echo "grim could not capture; see $OUT_DIR/grim.log"
  exit 1
fi
wait "$SHELL_PID" 2>/dev/null || true

python3 - "$SHOT" <<'PY'
import sys, zlib, struct

path = sys.argv[1]
with open(path, 'rb') as fh:
    data = fh.read()

# Minimal PNG reader: no Pillow on a bare runner. Only 8-bit RGB/RGBA from
# grim is expected, which is the one case this needs to handle.
pos, width, height, depth, color = 8, 0, 0, 0, 0
idat = bytearray()
while pos < len(data):
    (length,) = struct.unpack('>I', data[pos:pos + 4])
    kind = data[pos + 4:pos + 8]
    body = data[pos + 8:pos + 8 + length]
    if kind == b'IHDR':
        width, height, depth, color = struct.unpack('>IIBB', body[:10])
    elif kind == b'IDAT':
        idat += body
    elif kind == b'IEND':
        break
    pos += 12 + length

if depth != 8 or color not in (2, 6):
    print(f'unexpected PNG format: depth={depth} color={color}')
    sys.exit(1)

channels = 3 if color == 2 else 4
raw = zlib.decompress(bytes(idat))
stride = width * channels
prev = bytearray(stride)
colors = set()
off = 0
for _ in range(height):
    ft = raw[off]
    line = bytearray(raw[off + 1:off + 1 + stride])
    off += 1 + stride
    for i in range(stride):
        a = line[i - channels] if i >= channels else 0
        b = prev[i]
        c = prev[i - channels] if i >= channels else 0
        if ft == 1:
            line[i] = (line[i] + a) & 0xFF
        elif ft == 2:
            line[i] = (line[i] + b) & 0xFF
        elif ft == 3:
            line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
        elif ft == 4:
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            line[i] = (line[i] + pr) & 0xFF
    for x in range(0, stride, channels):
        colors.add(bytes(line[x:x + 3]))
        if len(colors) > 8:
            break
    prev = line
    if len(colors) > 8:
        break

print(f'{width}x{height}, distinct colors (capped): {len(colors)}')
if len(colors) <= 1:
    print('FAIL: the frame is a single flat color, so nothing was presented')
    sys.exit(1)
print('PASS: the frame has content')
PY
