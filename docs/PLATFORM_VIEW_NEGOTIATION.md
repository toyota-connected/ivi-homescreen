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
| `IHS_PV_KIND_SOFTWARE_SHM` | `1u << 2` | CPU-written pixels in shared memory |
| `IHS_PV_KIND_TEXTURE_DMABUF_IMPORT` | `1u << 0` | a dma-buf the shell imports as a texture |
| `IHS_PV_KIND_DRM_PLANE` | `1u << 1` | a dma-buf scanned out directly on a KMS overlay plane |

`IHS_PV_KIND_NONE` is `0` and is never granted.

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

Consequences worth stating plainly, because they are what a plugin plans
against:

- **`SOFTWARE_SHM` is the only universal kind.** It is the floor and is always
  offered. A plugin that implements the floor always has a path.
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
| `IHS_PV_SYNC_EXPLICIT_REQUIRED` | the grant must honor explicit sync |

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
| `IHS_PV_ERR_UNSUPPORTED` | `-3` | the requirement excludes even the floor |
| `IHS_PV_ERR_NO_REGISTRY` | `-4` | registry unavailable (headless) |

`IHS_PV_OK` does **not** mean you got the kind you asked for. It means a grant
was made — read `grant.kind` to find out which, and be prepared for the floor.
`IHS_PV_ERR_UNSUPPORTED` is returned only when the requirement set excludes even
`SOFTWARE_SHM`, i.e. the plugin cleared the floor bit and no higher kind could
be granted.

An empty requirement format list means "any the backend offers", which resolves
to `caps.formats[0]`. The grant's kind-specific payload is read through the
accessors declared after `ihs_pv_negotiate`, and is valid only for the current
grant on that view.

## The renegotiation contract

### What triggers it

A grant is not permanent. The shell revokes and re-offers when the surface
underneath it changes — an output or mode change, a plane becoming unavailable,
a DRM lease withdrawn. `App::RenegotiateView` (`shell/app.cc`) drives this from
output transitions.

Notification is **per view, not global**. `RenegotiateView` walks
`registry->InstanceIds()` and calls `PlatformViewRegistry::Renegotiate(id)` for
the views on the output that changed, deliberately not `RenegotiateAll()` — a
second view on another output of the same display has not moved and must not be
told it has.

### Where it runs

On the **platform thread**. The registry is platform-thread-only, so
`RenegotiateView` hops via `PostToPlatformThread` before touching it. Your
`renegotiate` callback therefore runs on the platform thread; do not block it.

### What state the old grant is in

Stale. Treat the previous grant and its kind-specific payload as no longer
valid the moment `renegotiate` fires — the payload accessors are documented as
valid only for the current grant.

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
