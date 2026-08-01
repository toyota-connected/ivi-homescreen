# multi_touch_test

Integration test app for **10-finger multi-touch delivery** against the
ivi-homescreen embedder, plus a uinput injector that simulates a 10-slot
MT protocol B touch controller — no hardware needed.

The contract under test is the whole touch path:

```
kernel evdev (MT slots, SYN_REPORT = one hardware scan)
  -> libinput TOUCH_DOWN/MOTION/UP + TOUCH_FRAME
  -> wl_touch.down/motion/up + wl_touch.frame        (Wayland backends)
     or DrmSeat / SoftwareSeat directly              (drm / software backends)
  -> FlutterEngineSendPointerEvent (one call per touch frame)
  -> one PointerDataPacket -> one UI-thread task
  -> PointerDown/Move/Up/Cancel on a raw Listener
```

The app uses a full-screen `Listener` — no `GestureDetector`, no gesture
arena — so the raw stream reaches the checks unmodified.

## Checks

| # | Check | What it validates |
| --- | --- | --- |
| C1 | concurrency | ≥ `EXPECT_FINGERS` (10) simultaneous contacts with that many distinct device ids |
| C2 | legality | per-device phase machine: no down-while-down, no move/up/cancel-while-up. Catches lost transitions and the historical cancel bug (only device 0 cancelled → 9 contacts stuck down → next session violates) |
| C3 | churn | ≥ `EXPECT_CHURN_IDS` (24) distinct device ids across the run. The pre-fix Wayland path indexed a fixed `surface_x[kMaxTouchFinger]` array by the `wl_touch` id — an out-of-bounds write for id ≥ 10. **Wayland backends only**; see [C3 only applies to the Wayland backends](#c3-only-applies-to-the-wayland-backends) |
| C4 | frame batch | contacts in one hardware scan arrive with one shared timestamp (the embedder stamps the batch once). Mean move-group size during the synchronized 10-finger drag must be ≥ `EXPECT_BATCH_MEAN` (6.0). An unbatched embedder stamps each contact separately → mean ≈ 1 |
| C5 | cancel | after a compositor cancel, every down contact receives `PointerCancel` within 500 ms. Advisory until a cancel is observed (trigger one manually via a compositor system gesture) |

Whenever all contacts lift, two lines are printed:

```
MTT-CHECKS: c1=pass c2=pass c3=FAIL c4=pass c5=FAIL
MTT-SUMMARY: {json}
```

`MULTI_TOUCH_TEST: PASS|FAIL` is printed by the Self-check button, or
automatically with `--dart-define=SELFCHECK_AFTER_S=<seconds>`.

`MTT-SUMMARY` arrives whole, on one line, and parses. A log record is capped at
`IHS_LOG_TEXT_CAPACITY` and the encoded summary is longer than that, but the
logging library splits an over-length message across records and rejoins it
before any sink sees it, so what lands in the log is a single line.

`MTT-CHECKS` remains the easier thing to scrape — the five verdicts, no JSON
parser needed — and `MULTI_TOUCH_TEST: PASS|FAIL` easier still. The summary
orders the verdict flags first, which no longer matters for surviving a cut and
is simply a readable order.

Verified on a Raspberry Pi 4 over `drm-kms-egl`: two summary lines from a touch
session, both parsed by `json.load`, 14 keys each.

## Expected results

| Embedder | C1 | C2 | C3 † | C4 (mean batch) |
| --- | --- | --- | --- | --- |
| pre touch-frame-batching | pass | pass* | Wayland: **undefined behaviour** (OOB write for id ≥ 10) — drm/software: n/a | **fail** (≈ 1.0) |
| with touch-frame-batching | pass | pass | Wayland: pass — drm/software: n/a | pass (≈ 10) |

\* C2 fails pre-fix if a compositor cancel occurs mid-session (only device 0
was cancelled, at the mouse position).

† C3 as written can only be reached on the Wayland backends. On drm/software it
reports the panel's slot count and stops there, by design — see below.

### C3 only applies to the Wayland backends

The device id a contact arrives with is not the same quantity on every backend:

| Backend | Flutter device id | Range |
| --- | --- | --- |
| `wayland-egl`, `wayland-vulkan` | the compositor's `wl_touch` point id (`display.cc`) | unbounded — a compositor may keep incrementing |
| `drm-kms-egl`, `drm-kms-vulkan` | the libinput MT slot, `pe.device = ev.slot` (`drm_seat.cc`) | 0 .. slots-1, recycled |
| `software` | the libinput seat slot, range-checked against `kMaxTouchSlots` = 16 (`software_seat.cc`) | 0 .. 15, recycled |

C3 exists because an unbounded id was once used to index a fixed array, and
that array was Wayland's: `wl_fixed_t surface_x[kMaxTouchFinger]` in the
`touch_` struct in `display.h`, indexed by the `wl_touch` id. No other backend
ever had it — before the fix, that was the only file in `shell/` containing it.
Only the Wayland path carries an unbounded id, so only there can the distinct-id
count pass 24, and only under a compositor that does not recycle ids. On drm/software the id *is* a slot: it cannot exceed the controller's slot
count however much the fingers churn, so C3 tops out at that count and never
reaches the threshold. Neither backend can suffer the bug C3 guards against,
because neither has an unbounded id to overflow with.

Treat C3 as not applicable there rather than as a failure. Measured on a
Raspberry Pi 4 driving the 800x480 DSI panel (`ft5x06`, 10 slots) on
`drm-kms-egl`, with the injector's churn phase advancing kernel tracking ids to
40: `distinct_ids_total` = 10.

## Building

```sh
cd test/integration/multi_touch_test
flutter pub get
flutter build bundle \
    --dart-define=SELFCHECK_AFTER_S=60   # optional, for unattended runs
```

Point ivi-homescreen at `build/flutter_assets` via `-b`.

## Driving it

### Simulated controller (uinput)

```sh
pip install evdev
sudo ./tools/inject_ten_finger.py --width 1920 --height 1080
```

Needs root (or an `input`-group user with `/dev/uinput` access) and a
compositor that picks up hotplugged input devices. Adjust `--width/--height`
to the output resolution so injected coordinates land on the app. Phases:

1. **A** — press fingers 0..9 one per scan, hold (C1)
2. **B** — synchronized 10-finger drag, 250 scans at 125 Hz (C4)
3. **C** — staggered lift
4. **D** — 30 rapid taps; tracking ids grow monotonically past 10 (C3)
5. **E** — 3× quick full-hand tap

#### Spanned multi-monitor desktops

A nesting compositor (mutter, KWin) maps an unassociated touch device across
the **whole logical desktop**, so contacts sized to one output scatter onto
the neighbours — e.g. on a 2560+1280 dual-head only 6 of 10 fingers land on a
2560-wide fullscreen app. Pass the full span and the app output's offset so
every contact lands on the app:

```sh
sudo ./tools/inject_ten_finger.py --width 2560 --height 1440 \
    --desktop-width 3840 --desktop-height 1440 --output-x 0 --output-y 0
```

Simpler alternatives: disable the second monitor, or drive the **drm/software
backend on a VT** (below), where libinput feeds the seat directly and no
per-output mapping applies — none of these flags are needed there.

### Real hardware

Any ≥ 10-point panel. On a Wayland backend, C3 requires enough finger churn to
advance the compositor's touch ids past 24 — tap repeatedly. Compositors that
reuse the lowest free id (Weston, wlroots) will not grow ids by design; lower
`EXPECT_CHURN_IDS` for those.

What the injector can and cannot do for C3 differs by backend. On drm/software
its growing kernel tracking ids never reach the app at all: libinput maps them
onto seat slots first, and the slot is what becomes the Flutter device id, so
driving tracking ids to 40 on a 10-slot panel still yields 10 distinct ids. On
a Wayland backend it does automate the churn — phase D taps 30 times — but the
ids the app sees are the compositor's to allocate, so whether they grow past 24
remains the compositor's behavior and not something the injector can force.

## Why frame batching matters (the "engine interruption" question)

`FlutterEngineSendPointerEvent` turns **each call** into one
`PointerDataPacket` and posts it as **one task to the UI thread**
(`Shell::OnPlatformViewDispatchPointerDataPacket` →
`RunNowAndFlushMessages(ui_task_runner, ...)`). The embedder profile uses
`DefaultPointerDataDispatcher` — there is no engine-side vsync buffering
(that's Android-only `SmoothPointerDataDispatcher`), so batching is entirely
the embedder's job.

Per-contact submission on a 10-finger controller scanning at 250 Hz is
2 500 UI-thread task posts per second, each interleaving with frame
production. Batching on the protocol's atomicity boundary
(`wl_touch.frame` / libinput `TOUCH_FRAME` — everything between two markers
is one logically-simultaneous hardware scan) reduces that to 250, with one
lock acquisition, one main-loop wake, and one shared timestamp per scan.
