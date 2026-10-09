# Platform-view negotiation

Normative reference for the two things `shared/include/ihs/platform_view.h`
cites but does not fully state: the **kind matrix** — which surface kinds a
given backend can grant — and the **renegotiation contract** — what a plugin
must do when a grant goes stale.

This describes what the shell implements today. Where a statement is a property
of the current implementation rather than a guarantee of the ABI, it says so.

## Surface kinds

`IhsPvKind` is a bitmask (`platform_view.h`):

| Kind | Bit | What the plugin submits |
| --- | --- | --- |
| `IHS_PV_KIND_SOFTWARE_SHM` | `1u << 2` | CPU-written pixels in buffers the host allocates (see [SOFTWARE_SHM buffers](#software_shm-buffers)) |
| `IHS_PV_KIND_TEXTURE_DMABUF_IMPORT` | `1u << 0` | a dma-buf the shell imports as a texture |
| `IHS_PV_KIND_DRM_PLANE` | `1u << 1` | a dma-buf scanned out directly on a KMS overlay plane |
| `IHS_PV_KIND_TEXTURE_EGL_IMAGE` | `1u << 3` | an EGLImage on the backend's display, in an `IhsLayer`'s `image` (ABI 1.16) |
| `IHS_PV_KIND_TEXTURE_VK_IMAGE` | `1u << 4` | a VkImage on the backend's device, in an `IhsLayer`'s `vk_image` (ABI 1.21) |

`IHS_PV_KIND_NONE` is `0` and is never granted.

The two image kinds are not negotiated. A backend that samples them reports the
bit in `IhsPvCapabilities::kinds`, and any view on it may then submit image
layers through `ihs_pv_submit_layers`. They are always composited, never put
on a plane.

## The kind matrix

`HostQueryCapabilities`
(`shell/platform/homescreen/platform_views/platform_view_host.cc`) builds the
offered set from what the **active backend** exposes, not from a backend
identity table:

| Condition on the active backend | Kind offered |
| --- | --- |
| always | `SOFTWARE_SHM` |
| `GetVulkanContext()` succeeds | `TEXTURE_DMABUF_IMPORT` |
| `GetEglContext()` succeeds (`IVI_HAVE_EGL` builds) | `TEXTURE_DMABUF_IMPORT` |
| `GetEglContext()` succeeds **and** `egl.gbm_device != nullptr` | `DRM_PLANE` |
| `GetEglContext()` succeeds on a backend that is not also Vulkan | `TEXTURE_EGL_IMAGE` |
| `GetVulkanContext()` succeeds | `TEXTURE_VK_IMAGE` |

Consequences worth stating plainly, because they are what a plugin plans
against:

- **`SOFTWARE_SHM` is the only universal kind.** It is the floor and is always
  offered. A plugin that implements the floor has a path wherever the host can
  allocate its buffers: GBM on a render node, else the backend's own GBM
  device. Without libgbm, or with neither, the grant is refused with
  `IHS_PV_ERR_UNSUPPORTED`.
- **`TEXTURE_DMABUF_IMPORT` is not universal.** It requires a GPU context. A
  build running the software backend with neither a Vulkan nor an EGL context
  offers `SOFTWARE_SHM` alone. Do not assume dma-buf import is available without
  querying.
- **`DRM_PLANE` is the narrowest.** It needs a GBM device, which the DRM-KMS-EGL
  backend has and `wayland-egl` does not — `wayland-egl` leaves `gbm_device`
  null, so it offers dma-buf import but not plane scanout.

Query it, do not infer it:

```c
IhsPvCapabilities caps = { .struct_size = sizeof caps };
if (ihs_pv_query_capabilities(&caps) != IHS_PV_OK) { /* no backend yet */ }
if (caps.kinds & IHS_PV_KIND_DRM_PLANE) { /* zero-copy path available */ }
```

`caps.backend_key` (`"wayland-egl"`, `"drm-kms-vulkan"`,
`"wayland-leased-drm-vulkan"`, …) is **informational**. The registry is
string-keyed and new backends are added without an ABI change, so switching on
it is a bug waiting for the next backend. Select a path by testing the kind
bits and by calling the context accessor you want — each returns
`IHS_PV_ERR_UNSUPPORTED` when the active backend does not provide it.

### Formats

`caps.formats` / `caps.format_count` are populated **only when a dma-buf kind
was offered**. Advertising formats for a capability the backend does not have
would hand a plugin a format it can never submit, so a software-only backend
reports none.

The current set (`kDmabufImportFormats`) is four 8888 formats, all with the
linear modifier (`0`):

| fourcc | DRM format |
| --- | --- |
| `XR24` | `DRM_FORMAT_XRGB8888` |
| `AR24` | `DRM_FORMAT_ARGB8888` |
| `XB24` | `DRM_FORMAT_XBGR8888` |
| `AB24` | `DRM_FORMAT_ABGR8888` |

This list is implementation detail and may grow. Read it from the capabilities
rather than hard-coding it.

### Explicit sync

`caps.explicit_sync` is set to `1` only when the shared Vulkan device advertises
`VK_KHR_external_semaphore_fd`, which is what the compositor's wait path
(`TakeAcquireFenceFd` → `vkImportSemaphoreFdKHR`) needs to import a producer's
`sync_file` as a semaphore.

`IhsPvSync` requests interact with it:

| Request | Behavior |
| --- | --- |
| `IHS_PV_SYNC_IMPLICIT` | no fence exchange |
| `IHS_PV_SYNC_EXPLICIT_PREFERRED` | explicit if available, silently implicit if not |
| `IHS_PV_SYNC_EXPLICIT_REQUIRED` | the grant must honor explicit sync; negotiation fails with `IHS_PV_ERR_UNSUPPORTED` when the backend has none, before anything is granted |

A grant may honor a **weaker** mode than requested unless `EXPLICIT_REQUIRED`
was set. A plugin that cannot cope with implicit sync must ask for
`EXPLICIT_REQUIRED` rather than asking for `PREFERRED` and assuming.

## Negotiation

`ihs_pv_negotiate` scores the requirements against the active backend and live
capability probes — plane availability, format and modifier intersection — and
writes the best grant to `out`.

Result codes (`IhsPvResult`):

| Code | Value | Meaning |
| --- | --- | --- |
| `IHS_PV_OK` | `0` | a grant was made, possibly the software floor |
| `IHS_PV_ERR_INVALID` | `-1` | null or oversized arguments, bad `struct_size` |
| `IHS_PV_ERR_NO_BACKEND` | `-2` | no active backend to negotiate against |
| `IHS_PV_ERR_UNSUPPORTED` | `-3` | the requirement excludes even the floor, or the floor's buffers cannot be allocated |
| `IHS_PV_ERR_NO_REGISTRY` | `-4` | registry unavailable (headless) |

`IHS_PV_OK` does **not** mean you got the kind you asked for. It means a grant
was made — read `grant.kind` to find out which, and be prepared for the floor.
`IHS_PV_ERR_UNSUPPORTED` is returned when the requirement set excludes even
`SOFTWARE_SHM`, i.e. the plugin cleared the floor bit and no higher kind could
be granted, and when `SOFTWARE_SHM` is the kind chosen but the host cannot
allocate its buffers.

An empty requirement format list means "any the backend offers", which resolves
to `caps.formats[0]`. The grant's kind-specific payload is read through the
accessors declared after `ihs_pv_negotiate`, and is valid only for the current
grant on that view.

### Which kind wins

With no preference expressed, the first of these that both sides offer:

| order | kind |
| --- | --- |
| 1 | `TEXTURE_DMABUF_IMPORT` |
| 2 | `DRM_PLANE` |
| 3 | `SOFTWARE_SHM` |

**That is a tie-break, not a performance ranking.** Direct scanout is the faster
path where it applies: measured on a composited platform view it nearly halved
total frame time, with raster identical between the two and the whole difference
being the composite step the plane path skips (ivi-homescreen#669). Import is
first because it is the *certain* path — a plane may not be allocatable for a
given frame and the scene falls back to composition anyway — so ranking the
certainty first keeps a producer from negotiating for scanout and silently
getting composition.

To prefer scanout, name it (ABI 1.17):

```c
req.kinds = IHS_PV_KIND_DRM_PLANE | IHS_PV_KIND_TEXTURE_DMABUF_IMPORT |
            IHS_PV_KIND_SOFTWARE_SHM;
req.preferred_kind = IHS_PV_KIND_DRM_PLANE;   /* honored if grantable */
```

`preferred_kind` is honored when it names exactly one kind that both
`req.kinds` and `caps.kinds` include; otherwise the order above decides. So a
preference never costs the producer its fallback, and is free to ask for. Before
1.17 the only way to reach `DRM_PLANE` was to request it *alone* — the order put
import first, and nearly every producer lists import since it is the portable
path — which meant giving up the fallback entirely (ivi-homescreen#673).

#### Why the order was not simply changed

Reordering `kKindPriority` to put `DRM_PLANE` first would have given every
producer that lists both kinds the faster path with no opt-in and no ABI change,
which is a real advantage over an opt-in field: nothing gets faster here until a
producer is updated. It was rejected for four reasons, recorded so the next
reader does not have to re-derive them (ivi-homescreen#673):

- **It changes behavior under producers silently.** A producer tuned for import
  — buffer counts, formats, sync strategy — would get a path it was never
  written or tested against, with no version or capability bit marking the
  change.
- **A plane grant is a maybe, an import is a certainty.** Where no plane is
  allocatable for a frame the scene composites anyway. As a default that means a
  producer believes it negotiated scanout and intermittently gets composition,
  which is the hardest case to reason about for frame timing and for the
  `IHS_PV_PRESENTED_ZERO_COPY` flag. As an explicit preference it is an informed
  choice: note that this is *relocated*, not solved — a producer that prefers a
  plane still composites on a frame with none available.
- **Only one backend offers the kind.** `DRM_PLANE` needs a GBM device, so
  `drm-kms-egl` offers it and the Wayland and Vulkan backends do not (see the
  kind matrix above). Reordering would therefore change behavior on one backend
  and not the others, with nothing in the ABI declaring that.
- **Planes are scarce hardware.** Granting them by default makes allocation
  order-dependent: whichever views negotiate first consume the planes and later
  ones quietly do not, which no part of this ABI can express.

## SOFTWARE_SHM buffers

The host allocates them (ABI 1.19, #721). `ihs_pv_grant_shm_slots` returns
their fds, borrowed, and their stride; `ihs_pv_grant_shm_fd` is the first of
them. There are two, so the producer writes one while the compositor samples
the other, which it does in place, without a copy.

- Each is a LINEAR dma-buf of the granted format at the view's size, with room
  for one more row, opened read-write. A view with no size yet has none until
  the resize that gives it one.
- The producer maps it, brackets its writes with `DMA_BUF_IOCTL_SYNC`, and
  submits it with `ihs_pv_submit`: one plane, a dup of its fd, offset 0, its
  stride, `buffer_id` its index, acquire fence -1. A frame naming any other
  buffer for that index is refused.
- Every submit returns a release fence, an eventfd. It fires once a later
  frame replaced the buffer and the composites that sampled it are done. The
  producer writes the buffer again only after that.
- A resize revokes the grant: the host calls `resize`, then `renegotiate`, and
  the producer negotiates there for buffers of the new size.
- These buffers are always composited, never placed on a plane, and
  `ihs_pv_submit_layers` does not take them.

## The renegotiation contract

### What triggers it

A grant is not permanent. The shell re-offers when the surface underneath it
changes — an output or mode change, a plane becoming unavailable, a DRM lease
withdrawn. `App::RenegotiateView` (`shell/app.cc`) drives this from output
transitions.

Re-offering is notification only: `PlatformViewRegistry::Renegotiate` invokes
the callback and nothing else. The old grant is not torn down behind the
plugin's back — it is replaced when the plugin calls `ihs_pv_negotiate` again,
and kept if it never does.

Notification is **per view, not global**. `RenegotiateView` walks
`registry->InstanceIds()` and calls `PlatformViewRegistry::Renegotiate(id)` for
the views on the output that changed, deliberately not `RenegotiateAll()` — a
second view on another output of the same display has not moved and must not be
told it has.

### Asking for one

A producer can also ask, with `ihs_pv_request_renegotiate(view)` (ABI 1.18).
It schedules the same per-view callback on the platform thread and returns
immediately; it is callable from any thread, like `ihs_pv_submit`.

This covers the case the triggers above do not: a producer whose submits keep
failing concludes its grant is dead and stops submitting, and no output change
is coming to revive the view. Asking is not a promise of a different path — the
host may grant the same kind again.

### Where it runs

On the **platform thread**. The registry is platform-thread-only, so
`RenegotiateView` hops via `PostToPlatformThread` before touching it. Your
`renegotiate` callback therefore runs on the platform thread; do not block it.

### What state the old grant is in

Stale. Treat the previous grant and its kind-specific payload as no longer
valid the moment `renegotiate` fires — the payload accessors are documented as
valid only for the current grant. The pointers do not become invalid at that
instant (nothing revoked them), but what they describe no longer matches the
surface the shell is serving.

### What the plugin must do

Either:

1. call `ihs_pv_negotiate` again and rebuild against the new grant, or
2. fall back to the software floor.

`ihs_pv_negotiate` is explicitly re-callable for this purpose.

### If the plugin does nothing

Nothing crashes, and there is no diagnostic. The view keeps its stale grant and
submits against a surface path the shell is no longer serving, which typically
shows up as a blank or frozen view rather than an error. Implementing
`renegotiate` is optional in the ABI but not optional in practice for a plugin
that wants to survive an output change.

A plugin that registers **no** `renegotiate` callback is counted as
"nothing to notify": `PlatformViewRegistry::Renegotiate` returns `false` both
when no instance holds the id and when the view registered no callback, and the
caller uses that to distinguish "nothing to notify" from "notified". The
per-view debug line `renegotiated N/M platform view(s)` reports the ratio.

## Teardown

Not part of renegotiation, but the adjacent lifecycle rule: on dispose the
registry invokes `IhsPvCallbacks::dispose` **first** and releases the grant
**after** it returns. So the grant is still valid inside `dispose`, and a plugin
can flush or release its own resources against it there.

## See also

- `shared/include/ihs/platform_view.h` — the ABI itself and the per-view lifecycle
- `docs/PLUGIN_ABI.md` — boundary rules for out-of-tree plugins
