#!/usr/bin/env python3
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
"""Replay driver_probe::Resolve() against drm_info dumps.

Fetch the corpus once:
    curl -fsSL -o /tmp/drmdb.tar.gz https://drmdb.emersion.fr/snapshot.tar.gz
    mkdir -p /tmp/drmdb && tar -C /tmp/drmdb -xzf /tmp/drmdb.tar.gz

Then:
    scripts/replay_drmdb.py --input-dir /tmp/drmdb

Output:
  - Per-driver summary: how many dumps resolve to planes vs. gl, how many
    expose explicit-sync props, async-flip cap, etc.
  - Drivers where compositor=auto always resolves to gl (candidates for
    documentation — users on those drivers should expect fallback).
  - Drivers with mixed plane/gl outcomes (candidates for overlay_planes
    quirks in driver_probe.cc — one chip in the family has no overlays).

The logic mirrors shell/backend/drm_kms_egl/driver_probe.cc so discrepancies
between this script and the C++ probe indicate drift in either side.
"""

import argparse
import collections
import json
import pathlib
import sys

# ── DRM fourcc helpers ───────────────────────────────────────────────────
DRM_FORMAT_XRGB8888 = 0x34325258  # 'XR24'
DRM_FORMAT_XBGR8888 = 0x34324258  # 'XB24'
DRM_FORMAT_ARGB8888 = 0x34325241  # 'AR24'
DRM_FORMAT_ABGR8888 = 0x34324241  # 'AB24'
DRM_FORMAT_RGB565 = 0x36314752  # 'RG16'

# Mirrors driver_probe.cc:kFallbackChain — caller's preferred format tried
# first, then this order. Return 0 when nothing matches (hard failure).
FALLBACK_CHAIN = (
    DRM_FORMAT_XRGB8888,
    DRM_FORMAT_XBGR8888,
    DRM_FORMAT_ARGB8888,
    DRM_FORMAT_ABGR8888,
    DRM_FORMAT_RGB565,
)

# drm_info emits plane "type" as an enum with Overlay=0 Primary=1 Cursor=2,
# stored at properties.type.value.
PLANE_TYPE_OVERLAY = 0
PLANE_TYPE_PRIMARY = 1
PLANE_TYPE_CURSOR = 2


def fourcc_str(fcc: int) -> str:
    if fcc == 0:
        return "?"
    return "".join(chr((fcc >> (8 * i)) & 0xFF) for i in range(4))


# ── Resolved struct (mirrors driver_probe::Resolved) ─────────────────────
class Resolved:
    __slots__ = (
        "driver_name",
        "use_plane_compositor",
        "atomic_modeset",
        "allow_nonblock_modeset",
        "primary_format",
        "overlay_planes",
        "explicit_sync",
        "async_flip",
    )

    def __init__(self) -> None:
        self.driver_name = ""
        self.use_plane_compositor = False
        self.atomic_modeset = False
        self.allow_nonblock_modeset = False
        self.primary_format = 0
        self.overlay_planes = False
        self.explicit_sync = False
        self.async_flip = False


# ── Probe logic (mirrors driver_probe.cc) ────────────────────────────────
def _plane_type(p: dict) -> int:
    props = p.get("properties", {}) or {}
    t = props.get("type", {})
    if isinstance(t, dict):
        v = t.get("value")
        if isinstance(v, int):
            return v
    return -1


def _plane_has_prop(p: dict, name: str) -> bool:
    return name in (p.get("properties", {}) or {})


def _plane_formats(p: dict) -> set:
    return set(f for f in (p.get("formats", []) or []) if isinstance(f, int))


def resolve(dump: dict) -> Resolved:
    r = Resolved()

    drv = dump.get("driver", {}) or {}
    r.driver_name = drv.get("name", "") or ""

    caps = drv.get("caps", {}) or {}
    client_caps = drv.get("client_caps", {}) or {}

    # Match driver_probe::HasAtomicCap — probes CRTC_IN_VBLANK_EVENT, which
    # is required (though not sufficient) for atomic flip-event delivery.
    # Also require ATOMIC client cap — we set it in InitDrm.
    atomic_ok = bool(caps.get("CRTC_IN_VBLANK_EVENT", 0) == 1) and bool(
        client_caps.get("ATOMIC", False)
    )

    # Find primary + count overlays. The C++ probe filters by a specific
    # CRTC's possible_crtcs mask; for corpus statistics we accept any plane
    # in the dump (same driver family, same answer).
    planes = dump.get("planes", []) or []
    primary = None
    overlay_count = 0
    for p in planes:
        t = _plane_type(p)
        if t == PLANE_TYPE_PRIMARY and primary is None:
            primary = p
        elif t == PLANE_TYPE_OVERLAY:
            overlay_count += 1

    # use_plane_compositor: atomic + primary present + ≥1 overlay
    r.atomic_modeset = atomic_ok
    r.use_plane_compositor = atomic_ok and primary is not None and overlay_count >= 1
    r.overlay_planes = overlay_count >= 1

    # primary_format: mirrors driver_probe.cc pick_format() under kAuto —
    # preferred = XRGB8888 first, then walks FALLBACK_CHAIN in order.
    # Returns 0 when the primary advertises none of the known formats.
    if primary is not None:
        fmts = _plane_formats(primary)
        for f in FALLBACK_CHAIN:
            if f in fmts:
                r.primary_format = f
                break

    # If format resolution failed, the backend would fail InitGbm — so the
    # plane compositor can't run either. Keep use_plane_compositor honest.
    if r.primary_format == 0:
        r.use_plane_compositor = False

    # explicit_sync: primary plane has IN_FENCE_FD
    if primary is not None:
        r.explicit_sync = _plane_has_prop(primary, "IN_FENCE_FD")

    # async_flip: DRM_CAP_ASYNC_PAGE_FLIP
    r.async_flip = caps.get("ASYNC_PAGE_FLIP", 0) == 1

    # allow_nonblock_modeset: always False under kAuto — no info in drmdb
    # to override. Kept for structural parity.
    r.allow_nonblock_modeset = False
    return r


# ── Reporting ────────────────────────────────────────────────────────────
def _fmt_count(num: int, denom: int) -> str:
    return f"{num:4d}/{denom:<4d}"


def _fmt_distribution(rs: list) -> str:
    """Compact multi-format string, e.g. 'XR24:3 XB24:1' or 'XR24:5'."""
    ctr = collections.Counter(r.primary_format for r in rs)
    parts = []
    # Preserve FALLBACK_CHAIN order for readability, then append anything
    # else (unexpected / exotic) at the end, then "fail=0" last.
    seen = set()
    for fcc in FALLBACK_CHAIN:
        if ctr.get(fcc):
            parts.append(f"{fourcc_str(fcc)}:{ctr[fcc]}")
            seen.add(fcc)
    for fcc, n in ctr.most_common():
        if fcc == 0 or fcc in seen:
            continue
        parts.append(f"{fourcc_str(fcc)}:{n}")
    if ctr.get(0):
        parts.append(f"fail:{ctr[0]}")
    return " ".join(parts) if parts else "-"


def summarize(by_driver: dict) -> None:
    total = sum(len(v) for v in by_driver.values())
    print(f"# drmdb replay — {total} dumps across {len(by_driver)} drivers")
    print()
    print(
        f"{'driver':22s}  {'n':>4s}  {'planes':>9s}  {'atomic':>9s}  "
        f"{'overlay':>9s}  {'ifence':>9s}  {'async':>9s}  formats"
    )
    print("-" * 120)
    for driver in sorted(by_driver):
        rs = [r for _, r in by_driver[driver]]
        n = len(rs)
        planes = sum(1 for r in rs if r.use_plane_compositor)
        atomic = sum(1 for r in rs if r.atomic_modeset)
        overlay = sum(1 for r in rs if r.overlay_planes)
        ifence = sum(1 for r in rs if r.explicit_sync)
        asyncf = sum(1 for r in rs if r.async_flip)
        print(
            f"{driver:22s}  {n:4d}  "
            f"{_fmt_count(planes, n)}  "
            f"{_fmt_count(atomic, n)}  "
            f"{_fmt_count(overlay, n)}  "
            f"{_fmt_count(ifence, n)}  "
            f"{_fmt_count(asyncf, n)}  "
            f"{_fmt_distribution(rs)}"
        )


def flag_edge_cases(by_driver: dict) -> None:
    always_gl = []
    mixed = []
    fmt_failed = []
    non_xrgb = []
    for driver, entries in by_driver.items():
        rs = [r for _, r in entries]
        n = len(rs)
        yes = sum(1 for r in rs if r.use_plane_compositor)
        fails = sum(1 for r in rs if r.primary_format == 0)
        non_xrgb_count = sum(
            1 for r in rs if r.primary_format not in (0, DRM_FORMAT_XRGB8888)
        )
        if yes == 0:
            always_gl.append((driver, n))
        elif yes < n:
            mixed.append((driver, yes, n))
        if fails:
            fmt_failed.append((driver, fails, n))
        if non_xrgb_count:
            non_xrgb.append((driver, non_xrgb_count, n))

    if fmt_failed:
        print()
        print("# Drivers where primary format resolution FAILED (returns 0):")
        print("# (these would fail InitGbm with the old code — the new fallback")
        print("#  chain is the minimum to unblock them; if still failing, the")
        print("#  driver needs an exotic format we don't know)")
        for d, fails, n in sorted(fmt_failed):
            print(f"  - {d}  fail={fails}/{n}")

    if non_xrgb:
        print()
        print("# Drivers that auto-resolve to something OTHER than XR24:")
        print("# (without the extended fallback chain these would have crashed")
        print("#  at gbm_surface_create on the old hardcoded XRGB8888)")
        for d, c, n in sorted(non_xrgb):
            print(f"  - {d}  non-xrgb={c}/{n}")

    if always_gl:
        print()
        print("# Drivers where auto always resolves to GL fallback:")
        print("# (candidates to document — users on these drivers get GL; no hw planes)")
        for d, n in sorted(always_gl):
            print(f"  - {d}  (n={n})")

    if mixed:
        print()
        print("# Drivers with mixed plane/gl outcomes:")
        print("# (candidates for driver_probe.cc overlay-planes quirks — same driver")
        print("#  name, different chip; either blacklist per-device-data or accept split)")
        for d, yes, n in sorted(mixed):
            print(f"  - {d}  planes={yes}/{n}")


# ── Main ─────────────────────────────────────────────────────────────────
def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--input-dir",
        required=True,
        type=pathlib.Path,
        help="directory of drm_info JSON dumps (from drmdb snapshot)",
    )
    ap.add_argument(
        "--per-device",
        action="store_true",
        help="print resolved fields for every dump (noisy)",
    )
    ap.add_argument(
        "--driver",
        default=None,
        help="restrict analysis to a single driver name (e.g. amdgpu)",
    )
    args = ap.parse_args()

    if not args.input_dir.is_dir():
        print(f"error: {args.input_dir} is not a directory", file=sys.stderr)
        return 2

    by_driver = collections.defaultdict(list)
    parse_errs = 0
    for path in sorted(args.input_dir.rglob("*.json")):
        try:
            dump = json.loads(path.read_text())
        except (json.JSONDecodeError, OSError):
            parse_errs += 1
            continue
        r = resolve(dump)
        if args.driver and r.driver_name != args.driver:
            continue
        by_driver[r.driver_name or "(unknown)"].append((path.name, r))

    if parse_errs:
        print(f"# note: {parse_errs} files failed to parse", file=sys.stderr)

    if not by_driver:
        print("no dumps matched", file=sys.stderr)
        return 1

    summarize(by_driver)
    flag_edge_cases(by_driver)

    if args.per_device:
        print()
        print("# Per-device detail:")
        for driver in sorted(by_driver):
            for name, r in sorted(by_driver[driver]):
                print(
                    f"  {driver:20s}  {name:40s}  "
                    f"compositor={'planes' if r.use_plane_compositor else 'gl   '}  "
                    f"fmt={fourcc_str(r.primary_format):>4s}  "
                    f"overlay={'Y' if r.overlay_planes else 'n'}  "
                    f"ifence={'Y' if r.explicit_sync else 'n'}  "
                    f"async={'Y' if r.async_flip else 'n'}"
                )
    return 0


if __name__ == "__main__":
    sys.exit(main())