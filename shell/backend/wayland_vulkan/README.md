# wayland_vulkan backend

Vulkan presenter on Wayland. Pairs Mesa's `VK_KHR_wayland_surface` WSI
with the optional homescreen Vulkan compositor for platform views. This
README covers the parts that aren't obvious from the source — the
compositor-mediated vsync path, the present-mode lever, and the
profiler used to characterize them.

## Vsync via `wp_presentation_feedback`

Architecturally identical to `wayland_egl`'s wiring — same baton dance,
same Display event-thread dispatch — but with two practical differences
that turn out to matter:

1. **The hook site is `vkQueuePresentKHR`, not `eglSwapBuffers`.** The
   feedback object must be minted before the `wl_surface.commit` baked
   into the present call. `RequestPresentationFeedback()` is called in
   both Vulkan present paths (`PresentCallback` for non-compositor
   builds, `PresentLayersImpl` for `BUILD_COMPOSITOR=ON`).

2. **Mesa's Vulkan WSI already does its own `wp_presentation_feedback`
   bookkeeping internally** — that's how `vkAcquireNextImageKHR` and
   `vkQueuePresentKHR` know when to block under `VK_PRESENT_MODE_FIFO_KHR`.
   So when the embedder ALSO requests feedback, the compositor sees
   two outstanding feedback objects per commit. This is harmless
   (compositors are spec-required to answer all of them), but it does
   mean our wiring isn't adding any compositor-side pacing intelligence
   that wasn't already in play. What our wiring *does* add is delivering
   the timestamps to the Flutter Engine — Mesa keeps them to itself.

```
Flutter raster thread                Display event_thread_
─────────────────────                ────────────────────
PresentCallback / PresentLayersImpl
  RequestPresentationFeedback()      ───┐
  vkQueuePresentKHR (commits)           │  compositor scans out
  …                                     │
                                        ▼
                                     on_feedback_presented(tv_ns, refresh)
                                        ├─ update last_refresh_ns_
                                        ├─ exchange vsync_baton_
                                        ├─ store last_begin_frame_ns_
                                        └─ asio::post(strand, [&]{ OnVsync() })
```

`SetVsyncBaton` includes the same idle-wake kick as the EGL backend —
if no feedback is in flight (first frame, post-idle wake from input)
it drains the baton inline so Flutter doesn't sit forever waiting on
a commit that hasn't been scheduled.

## When `vsync_callback` is wired

`WaylandVulkanBackend::GetVsyncCallback()` returns `&VsyncTrampoline`
iff all three of:

1. The compositor advertised `wp_presentation`.
2. The announced `clock_id` is `CLOCK_MONOTONIC`.
3. `IVI_VK_VSYNC` is not `0`.

Otherwise it returns `nullptr` and the engine falls back to its
internal wall-clock scheduler. The decision is logged once at startup.

## Env vars

| Env | Default | Effect |
|------|---------|--------|
| `IVI_VK_VSYNC` | (on) | `0` disables both the `vsync_callback` wiring AND the per-commit `wp_presentation_feedback` requests, falling back to Flutter's internal wall-clock scheduler. Use to bisect pacing regressions or to A/B against the no-feedback path. |
| `IVI_VK_PROFILE` | (off) | Anything non-empty enables per-frame cadence profiling. Every 60 frames the profiler logs `profile (n=60): fps=X mean_interval=Yus max_interval=Zus present_failures=N discarded=M refresh=Rus flags=0xF pipeline_mean=Lus pipeline_max=Lmaxus buckets[60Hz/30Hz/20Hz/slow/idle]=…`. `pipeline_*` measures beginFrame-to-present latency (time from the `frame_start_time` handed to Flutter via OnVsync to the next `vkQueuePresentKHR` return). |
| `IVI_M2P_PROFILE` | (off) | Anything non-empty enables the **motion-to-photon** (input-to-scanout) profiler. Joins kernel input time from `zwp_input_timestamps_v1` (pointer/touch) with compositor scanout time from `wp_presentation` feedback. Reports two latencies per window: **frame-accurate** — each input is attributed to the frame whose build cutoff (the baton's `frame_start_time`) is at or after it, i.e. the frame that actually rendered it, giving true `present − input`; and **floor** — `present − input` for the first scanout at or after the input, which under-counts by the engine pipeline depth. Every 120 presents logs `[wayland] motion->photon: frame-accurate p50=Xms p99=Yms max=Zms \| floor p50=xms p99=yms \| n=N dropped=D`, plus a session summary (both metrics) at teardown. The frame-accurate metric is the input-side complement of the `pipeline_*` (beginFrame-to-present) latency above: together they span input → build → present. `IVI_PROFILE` enables it too. |
| `IVI_VK_PRESENT_MODE` | `fifo` | One of `fifo`, `fifo_relaxed`, `mailbox`, `immediate`. Selects the swapchain present mode if the compositor advertises it; otherwise falls back to FIFO with a warn. See "Present-mode interaction" below — `mailbox` is the only setting where `vsync_callback`'s contribution becomes large. |

## Present-mode interaction (the surprising part)

Profiling against KWin Wayland (wonderous-app, IVI_VK_PROFILE=1, 25s
sessions, 3 reps per cell) shows that under the default FIFO mode the
vsync_callback wiring **doesn't measurably move** the per-frame
interval histogram — but under MAILBOX it transforms the workload:

| Mode | Vsync | fps | 60Hz bucket | Discarded | pipeline_mean |
|---|---|---:|---:|---:|---:|
| `fifo` | ON | 56 ± 1 | 70-73% | 0 | 2.4-2.7ms |
| `fifo` | OFF | 55 ± 1 | 71-83% (wider variance) | 0 | — (no instrument) |
| `mailbox` | ON | 297-765 | 97-99% (artifact, see below) | 5957-17294 | 0.29-0.95ms |
| `mailbox` | OFF | 56-58 | 71-85% | 0 | — |

Three things worth understanding:

1. **Under FIFO, vsync_callback is invisible at the present layer.**
   Mesa's WSI already gates `vkAcquireNextImageKHR` / `vkQueuePresentKHR`
   strictly on compositor vblanks via its own internal feedback
   bookkeeping. The rasterizer can't outrun scanout regardless of
   what Flutter thinks the schedule is, so the present-interval
   histogram looks the same with or without our wiring. Justification
   for keeping the wiring on by default is parity with DRM and EGL +
   richer diagnostics in `IVI_VK_PROFILE` (the `flags`, `discarded`,
   `refresh`, and `pipeline_*` fields all require it).

2. **Under MAILBOX, vsync_callback is what lets Flutter exploit the
   lower-latency headroom.** Without it, Flutter's wall-clock scheduler
   produces ~60 frames/sec regardless of how willing the compositor
   is to drop them. With it, Flutter sees accurate vblank timestamps,
   schedules beginFrame immediately after each reported vblank, and
   the rasterizer runs at 300-700 fps. The compositor still presents
   at the panel's refresh rate; the "extra" frames are `discarded`
   (newer commit superseded them) and that's the point — input
   events get reflected in the next presented frame within one
   vblank, instead of waiting for an already-queued frame to age out.

3. **The MAILBOX "97-99% on-vblank" reading is partially a
   measurement artifact.** When the rasterizer fires
   `vkQueuePresentKHR` every ~1.3-3ms, every inter-present interval
   trivially lands in the ≤17ms bucket regardless of true compositor
   pacing — the bucket thresholds were calibrated against a 60Hz
   FIFO baseline. The real signal under MAILBOX is the
   `pipeline_mean` drop (2.5ms → ~0.5ms) and the rasterizer
   outpacing scanout: Flutter is reacting to vblanks within a
   millisecond instead of within a vblank-period.

For typical desktop / automotive workloads, FIFO + vsync ON is the
right default. MAILBOX + vsync ON is the right choice when input
responsiveness matters more than rasterizer power: telematics
dashboards reacting to vehicle data, low-latency map panning, etc.
MAILBOX + vsync OFF is a degenerate case — pick FIFO instead.

## Architectural notes

**Single-threaded vs platform-runner dispatch.** Same as
`wayland_egl`: the `on_feedback_presented` / `on_feedback_discarded`
listeners fire on `Display::event_thread_`, so `feedback_in_flight_`
is guarded by a mutex and `OnVsync` is always posted via the
platform runner's strand. The future refactor of moving Wayland
dispatch onto the platform task runner via asio applies here too.

**Per-commit feedback object.** Identical lifecycle to EGL —
`feedback_in_flight_` tracks outstanding objects so `StopVsyncMonitor`
can destroy them before engine teardown. Race-safe destroy in both
the listener paths and the shutdown path: the path that successfully
removes the object from `feedback_in_flight_` owns its destruction.

**Mesa's internal feedback dups our requests.** The compositor sees
two `wp_presentation_feedback` objects per commit — one minted by
the embedder, one by Mesa's WSI. The compositor answers both. No
behavioral impact, but `WAYLAND_DEBUG=client` traces show twice as
many feedback exchanges as committed frames; that's expected.

**`pipeline_mean` is an approximation under pipelining.** Flutter
maintains a small queue of in-flight frames. The instrument samples
`last_begin_frame_ns_` (the most recent `frame_start_time` handed
to `OnVsync`) against `vkQueuePresentKHR` return time, so a given
present may not correspond to the most recent OnVsync. The running
mean still tracks the difference between vsync-aware and wall-clock
scheduling under load — it's the right metric for comparing wiring
configurations, not for precise per-frame attribution.

---

## Benchmarks

Measured on KWin Wayland (Fedora 43, kernel 6.19, AMD RADV
RAPHAEL_MENDOCINO iGPU), `feat/vulkan-vsync-callback` branch,
running the flutter-wonderous-app bundle, 3 reps per configuration.
Window size 1024×768. Compositor's primary output 3440×1440 @ 60Hz.

### Methodology

`IVI_VK_PROFILE=1` adds a log line every 60 presented frames with
mean/max interval, present failures, compositor discards, the
compositor-reported refresh, a flag OR (`VSYNC|HW_CLOCK|HW_COMPLETION`),
the beginFrame-to-present latency mean+max, and the 5-bucket histogram
(matches the wayland_egl bucket thresholds). On clean shutdown, the
backend's dtor emits a one-line session summary covering the whole run.

The matrix above varies `IVI_VK_VSYNC` and `IVI_VK_PRESENT_MODE` so
the contribution of each lever is separable.

### Session summary — FIFO + vsync (default)

Aggregate across 3 sessions, ~75 seconds, ~4000 presented frames:

| Metric | Value |
|---|---:|
| Compositor refresh | 16.668 ms (60 Hz) |
| Weighted mean interval | ~17.5 ms (≈ 57 Hz under load) |
| Worst single interval | ~600-800 ms (engine pause / content load) |
| Compositor discards | 0 |
| Feedback flags (OR) | `0x7` (VSYNC \| HW_CLOCK \| HW_COMPLETION) |
| beginFrame-to-present mean | 2.4 - 2.7 ms |
| 60Hz on-vblank bucket | 69 - 73% |
| 30Hz bucket (one miss) | 26 - 29% |

The 60Hz bucket sits below the wayland_egl headline (97%) because the
Vulkan WSI's blocking interferes with Flutter's pacing during
interactive load — Flutter aims for the next vblank, Mesa releases
the swapchain image at the same instant, and the small extra
serialization shifts the bucket toward the 30Hz bracket. Steady-state
(idle scrolling, no input) the bucket recovers to mid-90s.

### Session summary — MAILBOX + vsync

| Metric | Value |
|---|---:|
| Compositor refresh | 16.668 ms (60 Hz) |
| Mean inter-present interval | ~1.3 - 3.4 ms (rasterizer outpaces scanout) |
| beginFrame-to-present mean | 0.29 - 0.95 ms (5-9× lower than FIFO) |
| Compositor discards | 5957 - 17294 per ~25s session |
| Frames per session | 7260 - 18720 |
| Feedback flags (OR) | `0x7` (still HW-vblank-confirmed) |

Discards are NOT failures — they're the deliberate effect of MAILBOX
replacing queued frames with newer ones. Each presented frame is still
hardware-vblank-aligned (`flags=0x7`); the difference is that the
*latest* state reaches the panel instead of the oldest queued state.

### Comparison to wayland_egl

| Aspect | wayland_egl | wayland_vulkan (FIFO) | wayland_vulkan (MAILBOX) |
|---|---|---|---|
| Vsync source | `wp_presentation_feedback` per commit | same | same |
| Backpressure | `eglSwapBuffers` blocks at commit time | `vkQueuePresentKHR` blocks on vblank | non-blocking; rasterizer runs free |
| Mesa-side feedback | n/a (EGL has no internal WSI feedback) | duplicates embedder request | duplicates embedder request |
| Typical 60Hz bucket | ~97% (steady) | ~70% (active load) | artifact bucket — see pipeline_mean instead |
| pipeline_mean | n/a (no instrument) | ~2.5 ms | ~0.5 ms |
| Discards under load | 0 (KWin) | 0 | thousands (expected) |
| Best for | balanced power / smoothness | parity with EGL, simplest model | low input-to-photon latency |

### Comparison to drm_kms_egl

DRM benchmark (from `shell/backend/drm_kms_egl/README.md`): amdgpu
RDNA / Polaris @ 240Hz, same wonderous bundle, `IVI_DRM_VSYNC=1`,
64,769-frame sample.

| Metric | DRM @ 240Hz | wayland_vulkan @ 60Hz FIFO | wayland_vulkan @ 60Hz MAILBOX |
|---|---:|---:|---:|
| Frame budget | 4.17 ms | 16.67 ms | 16.67 ms (compositor) |
| Native hit-rate | 98.0% | ~70% | n/a (artifact) |
| pipeline_mean | n/a | 2.5 ms | 0.5 ms |
| Discards | n/a (kernel can't drop) | 0 | thousands |

DRM and wayland_egl both have natural backpressure that bounds frame
rate to refresh. Vulkan under FIFO has *strict* backpressure that's
slightly more brittle under load. Vulkan under MAILBOX deliberately
gives up backpressure for input latency — different operating point.

---

## Known limitations / follow-ups

1. **VK_ERROR_SURFACE_LOST_KHR on compositors lacking dmabuf** — Mesa's
   Vulkan WSI requires either `zwp_linux_dmabuf_v1` or `wl_drm` to
   allocate swapchain images. Compositors built without dmabuf (some
   Weston/AGL configurations) will fail `vkGetPhysicalDeviceSurfaceFormatsKHR`
   with SURFACE_LOST. The backend pre-flights the dmabuf check at
   startup and emits a clear warning; if the assertion still fires,
   the failure exits cleanly with `exit(EXIT_FAILURE)` and a critical
   log line naming the missing protocol. See commit `dba2bef1`.
2. **MAILBOX discard rate is workload-dependent.** A wonderous-style
   animation-heavy bundle produces thousands of discards per session
   under MAILBOX. A static dashboard with infrequent updates would
   produce essentially zero. Discards aren't dropped frames — they're
   superseded ones — but they do represent GPU work that didn't
   reach the panel.
3. **Cross-clock translation not implemented** — same as wayland_egl,
   compositors announcing a `clock_id` other than `CLOCK_MONOTONIC`
   fall back to the wall-clock scheduler. No mainline compositor
   needs this today.
4. **Pre-existing tidy warnings on `wayland_vulkan.cc`** — three
   `bugprone-branch-clone` / `readability-inconsistent-declaration-parameter-name`
   complaints in `debugReportCallback`, `debugUtilsCallback`, and
   `CollectBackingStore` predate this work. The lint CI doesn't
   catch them because the lint workflow's build dir uses the EGL
   backend; left for a separate cleanup PR.
