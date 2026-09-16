# Scheduling a DRM/KMS shell

Applies to `drm-kms-egl` and `drm-kms-vulkan`. Both drive scanout directly, so
the frame deadline is the panel's refresh period and nothing downstream will
paper over a late frame: miss it and the flip repeats the previous one, which
reads as judder.

On a developer desktop the default scheduling is fine. On an integrated system
it is usually not, because the shell is competing with services that asked for
real time and got it.

## Give the shell the same policy as the compositor it replaces

A DRM/KMS shell takes the display from whatever compositor normally owns it, so
it should be scheduled like that compositor, not below it. Check what the
existing one asks for:

```sh
systemctl cat <compositor>.service | grep CPUScheduling
```

A typical answer is `CPUSchedulingPolicy=fifo` with `CPUSchedulingPriority=2`,
with the window-system and layer-manager services a rung below at 1. Match it:

```ini
[Service]
ExecStart=/usr/bin/homescreen -b /path/to/bundle --backend drm-kms-egl \
          --drm-device /dev/dri/card1 --drm-connector <connector>
CPUSchedulingPolicy=fifo
CPUSchedulingPriority=2
```

Every thread the engine spawns inherits the policy, `io.flutter.ui` and
`io.flutter.raster` included, which is the part that matters. Confirm it:

```sh
ps -eLo pid,tid,cls,rtprio,comm | awk -v m=$(systemctl show <unit> -p MainPID --value) '$1==m'
```

`cls` should read `FF` and `rtprio` the priority, on every row.

**`systemd-run -p CPUSchedulingPriority=` does not work for this.** Some
systemd versions validate the priority against the policy in effect when the
property is parsed rather than the one being set, and reject a non-zero value
with `Invalid CPU scheduling priority: 2`. Use a unit file. For a throwaway
one, write it to `/run/systemd/system/` — that is tmpfs, so it disappears on
reboot and installs nothing.

Do not reach for `chrt` on the command line instead. It applies to the process
it execs, which is easy to get wrong through a wrapper, and it leaves no record
of how the shell was meant to run.

## Two mechanisms, and which to use

`drm_kms_egl` also carries an in-process option, `IVI_DRM_RT=1`, which calls
`pthread_setschedparam` on the threads that matter and gives them *different*
priorities: rasterizer `SCHED_FIFO` 2, UI thread `SCHED_FIFO` 1, background
work `SCHED_BATCH`. It needs `CAP_SYS_NICE` (or root) or it fails silently with
`EPERM`, leaving every thread at `SCHED_OTHER` with no warning. See "Real-time
scheduling: capability setup" in that backend's README.

| | `CPUSchedulingPolicy=` in a unit | `IVI_DRM_RT=1` |
|---|---|---|
| scope | every thread the process spawns | rasterizer, UI, platform runner |
| shape | one priority for all of them | differentiated, background demoted |
| privilege | granted by systemd | needs `CAP_SYS_NICE` or root |
| visibility | declared in the unit | an env var, easy to lose |

Prefer the unit property when the shell is replacing a compositor that is
itself scheduled that way: it states the intent where an integrator will look,
survives a restart, and covers the worker threads too. Reach for `IVI_DRM_RT`
where you cannot control the unit, or where demoting background work matters
more than raising everything uniformly. Setting both is not additive —
`IVI_DRM_RT` re-sets per-thread priorities on top of whatever the unit gave
them — so pick one.

The figures below were measured with the unit property. `IVI_DRM_RT` was not
A/B'd on the same system; its own README notes the gain is small on an idle
machine and grows with competing load, which is consistent with what the unit
property did here.

## What it is worth

Measured with `test/integration/scroll_bench` on a 1280x720@60 panel, on a
system running a dozen other `SCHED_FIFO` services, with
`--drm-pipeline-depth 2` (see the `drm_kms_egl` README) already in effect:

| | dropped frames | raster p99 | vsync→build p99 |
|---|---|---|---|
| `SCHED_OTHER` | 0.11% | 6.03 ms | 2.82 ms |
| `SCHED_FIFO` priority 2 | 0.02% | 3.08 ms | 0.83 ms |

The remaining 0.02% is a single frame at startup while the pipeline fills, not
a steady-state drop. Scheduling delay is the metric that moves: the shell was
not being denied CPU time overall, it was being denied it *on time*.

## Priority is not a substitute for headroom

Real-time priority stops the shell from being preempted; it does not create
room in the frame. If the present path serializes the raster thread against
scanout, priority barely helps, because the thread is blocked rather than
runnable. Fix the pacing first (`--drm-pipeline-depth 2` on `drm_kms_egl`,
which applies only where the legacy present path drives scanout — the
`drm_kms_egl` README says how to tell; `drm_kms_vulkan` already pipelines
through its slot ring), then raise the priority. In the measurements above, pacing took dropped frames from 7.34% to
0.11% and priority took them from 0.11% to effectively zero — in that order.

## Watch the deadline, not the frame rate

`IVI_PROFILE=1` reports flip cadence:

```
[DrmVsync] profile (n=60): fps=58.98 mean_interval=16955us ... buckets[60Hz/30Hz/20Hz/slow/idle]=60/0/0/0/0
```

This says the flips are on time. It does **not** say each flip carried a new
frame — a repeated frame flips exactly on schedule. A shell that drops 7% of
its frames still reports 60/60 here. To see content cadence, build the app with
`FrameTiming` logging (`TIMINGS_FILE`, see the `scroll_bench` README) and look
at the gaps between successive `vsyncStart` values: anything near two refresh
periods is a repeated frame.
