#!/usr/bin/env python3
# Simulate a 10-slot multi-touch controller (MT protocol B) via uinput and
# drive the multi_touch_test scenario phases against a running compositor +
# ivi-homescreen (or against the drm/software backends, which read libinput
# directly). Everything the kernel emits between two SYN_REPORTs is one
# hardware scan; libinput turns that into TOUCH_* events + TOUCH_FRAME, and
# Wayland compositors into wl_touch.* + wl_touch.frame — so this exercises
# the embedder's frame-batching boundary exactly like real hardware.
#
# Phases:
#   A  press fingers 0..9 one per frame, hold          -> C1 concurrency
#   B  synchronized 10-finger drag, N frames           -> C4 frame batching
#   C  staggered lift, one per frame
#   D  churn: rapid 1-finger taps, tracking ids grow   -> C3 (>10 ids; the
#      monotonically past 10                              pre-fix embedder
#                                                         indexed a 10-slot
#                                                         array by id -> OOB)
#   E  3x quick 10-finger tap (down all / frame / up all / frame)
#
# Requires: python3-evdev (pip install evdev), and root or an 'input'-group
# user with /dev/uinput access. Give the compositor ~1s after device creation
# to pick the node up (udev settle).
#
# Usage:
#   sudo ./inject_ten_finger.py [--width 1920] [--height 1080]
#                               [--rate 125] [--drag-frames 250]
#                               [--churn 30] [--settle 1.5]
#
# On a spanned multi-monitor desktop a compositor maps an unassociated touch
# device across the *whole* logical desktop, so contacts sized to one output
# scatter onto the neighbours. Pass the full span and the target output's
# offset so every contact lands on the app's output:
#   sudo ./inject_ten_finger.py --width 2560 --height 1440 \
#                               --desktop-width 3840 --desktop-height 1440 \
#                               --output-x 0 --output-y 0
# (single-output or the drm/software backends need none of these.)

import argparse
import time

from evdev import UInput, AbsInfo, ecodes as e

SLOTS = 10


def make_device(width: int, height: int) -> UInput:
    cap = {
        e.EV_KEY: [e.BTN_TOUCH],
        e.EV_ABS: [
            (e.ABS_MT_SLOT, AbsInfo(0, 0, SLOTS - 1, 0, 0, 0)),
            (e.ABS_MT_TRACKING_ID, AbsInfo(0, 0, 65535, 0, 0, 0)),
            (e.ABS_MT_POSITION_X, AbsInfo(0, 0, width - 1, 0, 0, 0)),
            (e.ABS_MT_POSITION_Y, AbsInfo(0, 0, height - 1, 0, 0, 0)),
            (e.ABS_X, AbsInfo(0, 0, width - 1, 0, 0, 0)),
            (e.ABS_Y, AbsInfo(0, 0, height - 1, 0, 0, 0)),
        ],
    }
    return UInput(cap, name='mtt-ten-finger', version=0x1)


class Panel:
    """Minimal MT protocol B state machine over uinput."""

    def __init__(self, ui: UInput, rate_hz: float,
                 origin=(0, 0), bounds=None):
        self.ui = ui
        self.frame_dt = 1.0 / rate_hz
        self.next_tracking_id = 0
        self.slot_tid = [-1] * SLOTS   # -1 == empty
        self.cur_slot = -1
        self.down_count = 0
        # Layout coordinates are in target-output pixel space. origin is that
        # output's top-left within the logical desktop and bounds is the
        # desktop extent the device is declared over; _map() offsets a point
        # onto the target output and clamps it to the desktop. On a spanned
        # multi-monitor compositor (which maps an unassociated touch device
        # across the *whole* desktop) this keeps every contact on the app's
        # output instead of scattering onto the neighbours. Defaults (origin
        # 0,0 / bounds == output) are a no-op for single-output and the
        # direct-libinput backends.
        self.origin = origin
        self.bounds = bounds  # (max_x, max_y) or None == no clamp

    def _map(self, x: int, y: int):
        x += self.origin[0]
        y += self.origin[1]
        if self.bounds is not None:
            x = max(0, min(self.bounds[0], x))
            y = max(0, min(self.bounds[1], y))
        return x, y

    def _slot(self, s: int):
        if self.cur_slot != s:
            self.ui.write(e.EV_ABS, e.ABS_MT_SLOT, s)
            self.cur_slot = s

    def press(self, s: int, x: int, y: int):
        assert self.slot_tid[s] < 0, f'slot {s} already down'
        self._slot(s)
        tid = self.next_tracking_id
        self.next_tracking_id += 1
        self.slot_tid[s] = tid
        mx, my = self._map(x, y)
        self.ui.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, tid)
        self.ui.write(e.EV_ABS, e.ABS_MT_POSITION_X, mx)
        self.ui.write(e.EV_ABS, e.ABS_MT_POSITION_Y, my)
        if self.down_count == 0:
            self.ui.write(e.EV_KEY, e.BTN_TOUCH, 1)
        self.down_count += 1

    def move(self, s: int, x: int, y: int):
        assert self.slot_tid[s] >= 0, f'slot {s} not down'
        self._slot(s)
        mx, my = self._map(x, y)
        self.ui.write(e.EV_ABS, e.ABS_MT_POSITION_X, mx)
        self.ui.write(e.EV_ABS, e.ABS_MT_POSITION_Y, my)

    def lift(self, s: int):
        assert self.slot_tid[s] >= 0, f'slot {s} not down'
        self._slot(s)
        self.ui.write(e.EV_ABS, e.ABS_MT_TRACKING_ID, -1)
        self.slot_tid[s] = -1
        self.down_count -= 1
        if self.down_count == 0:
            self.ui.write(e.EV_KEY, e.BTN_TOUCH, 0)

    def frame(self):
        """SYN_REPORT: one hardware scan boundary."""
        self.ui.syn()
        time.sleep(self.frame_dt)


def finger_home(i: int, w: int, h: int):
    """Ten spread-out home positions, two rows of five."""
    col, row = i % 5, i // 5
    x = int(w * (0.12 + 0.19 * col))
    y = int(h * (0.35 + 0.30 * row))
    return x, y


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--width', type=int, default=1920,
                    help='target output width in px (where the app is '
                         'fullscreen); the finger layout is sized to this')
    ap.add_argument('--height', type=int, default=1080,
                    help='target output height in px')
    ap.add_argument('--desktop-width', type=int, default=None,
                    help='full logical desktop span width the compositor maps '
                         'the touch device across (default: --width). Set to '
                         'the sum of all monitors so contacts land on the app '
                         'output rather than scattering across a spanned '
                         'multi-monitor desktop')
    ap.add_argument('--desktop-height', type=int, default=None,
                    help='full logical desktop span height (default: --height)')
    ap.add_argument('--output-x', type=int, default=0,
                    help="target output's left edge within the desktop (px); "
                         'the offset applied to every injected contact')
    ap.add_argument('--output-y', type=int, default=0,
                    help="target output's top edge within the desktop (px)")
    ap.add_argument('--rate', type=float, default=125.0,
                    help='scan rate in Hz (frames per second)')
    ap.add_argument('--drag-frames', type=int, default=250)
    ap.add_argument('--churn', type=int, default=30,
                    help='number of churn taps in phase D')
    ap.add_argument('--settle', type=float, default=1.5,
                    help='seconds to wait after device creation')
    args = ap.parse_args()

    # Declare the device over the whole desktop so a device coordinate is a
    # desktop pixel (a spanned compositor maps device fraction across the full
    # logical desktop); the layout stays in target-output space and Panel
    # offsets it onto the output at (output-x, output-y).
    desktop_w = args.desktop_width or args.width
    desktop_h = args.desktop_height or args.height

    ui = make_device(desktop_w, desktop_h)
    print(f'created uinput device "mtt-ten-finger" '
          f'(output {args.width}x{args.height} at +{args.output_x}+'
          f'{args.output_y} on {desktop_w}x{desktop_h} desktop); settling '
          f'{args.settle}s ...')
    time.sleep(args.settle)

    p = Panel(ui, args.rate, origin=(args.output_x, args.output_y),
              bounds=(desktop_w - 1, desktop_h - 1))
    w, h = args.width, args.height
    home = [finger_home(i, w, h) for i in range(SLOTS)]

    # Phase A: press 0..9, one per frame.
    print('phase A: sequential 10-finger press')
    for i in range(SLOTS):
        p.press(i, *home[i])
        p.frame()
    for _ in range(10):  # hold
        p.frame()

    # Phase B: synchronized drag — all ten slots updated inside each frame.
    # This is the C4 discriminator: a frame-batching embedder delivers these
    # ten moves as one packet with one shared timestamp.
    print(f'phase B: synchronized drag, {args.drag_frames} frames')
    span_x = int(w * 0.06)
    for f in range(args.drag_frames):
        t = f / max(args.drag_frames - 1, 1)
        dx = int(span_x * (2 * t - 1))
        dy = int(h * 0.05 * (1 if (f // 40) % 2 == 0 else -1) * t)
        for i in range(SLOTS):
            x0, y0 = home[i]
            p.move(i, x0 + dx, max(0, min(h - 1, y0 + dy)))
        p.frame()

    # Phase C: staggered lift.
    print('phase C: staggered lift')
    for i in range(SLOTS):
        p.lift(i)
        p.frame()

    # Phase D: churn. Tracking ids grow monotonically, so libinput/compositor
    # touch ids can exceed any fixed 10-slot bound downstream.
    print(f'phase D: churn, {args.churn} taps '
          f'(tracking ids -> {p.next_tracking_id + args.churn})')
    for k in range(args.churn):
        x = int(w * 0.5) + (k % 7) * 13
        y = int(h * 0.5) + (k % 5) * 11
        p.press(0, x, y)
        p.frame()
        p.frame()
        p.lift(0)
        p.frame()

    # Phase E: quick full-hand taps — down-all / frame / up-all / frame.
    print('phase E: 3x 10-finger tap')
    for _ in range(3):
        for i in range(SLOTS):
            p.press(i, *home[i])
        p.frame()
        p.frame()
        for i in range(SLOTS):
            p.lift(i)
        p.frame()
        for _ in range(5):
            p.frame()

    ui.close()
    print('done — check the app scoreboard / MTT-SUMMARY lines, or run the '
          'app with --dart-define=SELFCHECK_AFTER_S=<n> for an automatic '
          'MULTI_TOUCH_TEST: PASS|FAIL verdict.')


if __name__ == '__main__':
    main()
