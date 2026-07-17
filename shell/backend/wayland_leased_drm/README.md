# wayland_leased_drm backend

Takes exclusive control of a DRM connector from a running Wayland compositor via
`wp_drm_lease_device_v1` (drm-lease-v1, staging), then scans out on the leased fd
through one of the existing DRM present paths.

The Wayland connection is used **only** to negotiate the lease and to watch for
its revocation. There is no `wl_surface` and no compositor-composited path: this
is not a Wayland client in the usual sense, it is a DRM client that asks a
compositor for a connector instead of grabbing a card. Input comes from evdev,
not from the compositor — with no surface there is no keyboard or pointer focus
to receive.

Target use cases: an instrument cluster or HUD panel leased from a primary IVI
compositor, and any deployment where one process must own a connector while a
compositor owns the rest of the card.

## Read this first: can you actually get a lease?

Two things must both be true, and the second one is what usually bites.

**The compositor must implement drm-lease-v1.** Per the wayland.app support
matrix: KWin, the wlroots family (Sway, labwc, niri, Wayfire, …), Mutter,
Hyprland and COSMIC do. **Weston does not**, and therefore neither do AGL's
Weston-derived compositors. If your target is agl-compositor, this backend is
inert without compositor-side work.

**The compositor must be willing to offer your connector.** This is the real
gate. The protocol comes from VR, and the wlroots family only offers a connector
for leasing when its EDID carries the **non-desktop** flag; a compositor claims
every ordinary connector for its own compositing. On a normal desktop the result
is that the global exists and offers nothing:

```
$ wayland-info | grep -A1 drm_lease
interface: 'wp_drm_lease_device_v1', version: 1, name: 27
        path: /dev/dri/card1
```

...while every connector reports `"non-desktop" (immutable): range [0, 1] = 0`,
and enumeration completes with **zero offers**. `non-desktop` is immutable — you
cannot set it at runtime. Getting a lease from such a host needs one of: an EDID
that trips the kernel's `EDID_QUIRK_NON_DESKTOP` list (`drm.edid_firmware=`), a
compositor patch/config, or a compositor built for the job.

Ask the binary rather than assuming:

```bash
homescreen --lease-list-connectors        # no bundle required
```

It probes read-only (stopping before `create_lease_request`, so it is safe
against a live session) and prints what the compositor will actually lease. Exit
codes are scriptable, and separate the two failures that look identical from the
outside:

| exit | meaning |
|---|---|
| 0 | connectors listed |
| 1 | reached a lease device, but it offers **nothing** — the non-desktop gate above |
| 2 | no Wayland session, or the compositor has no drm-lease-v1 |

## Architecture

```
main → BackendRegistry["wayland-leased-drm-{egl|vulkan|software}"]
  make_display:
    LeaseClient::Acquire(cfg) ─▶ LeaseHold { lease_fd, connector_id, name,
        │                                    card path, wl_display + monitor
        │                                    thread, revoked flag }
        ├── egl/vulkan ▶ DrmDisplay(AdoptFd, …, fd, hold)
        └── software ─▶ SoftwareDisplay + LeasedScanout{fd, connector_id,
                                                        revoked, owner}
  make_backend:
        ├── MakeDrmEglBackend(cfg, display)          [egl — reused verbatim]
        ├── MakeDrmVulkanBackend(cfg, display)       [vulkan — branches on
        │                                             display->adopted_fd()]
        └── DrmDumbSink::Create(fd, owner, conn, revoked)   [software]
```

Two axes, deliberately orthogonal:

- **Acquisition** — how the DRM fd is obtained: direct open (`drm-kms-*`) vs
  leased (here).
- **Renderer** — how frames are produced: EGL/GBM, Vulkan, or CPU + dumb buffers.

The lease only changes acquisition. That is why the egl tier's `make_backend` is
`MakeDrmEglBackend` **unchanged**: the adopted-fd path is a construction tag on
`DrmDisplay`, not a subclass, so the backend factory cannot tell the difference.

### Why compound registry keys

The renderer determines the display type (`DrmDisplay` vs `SoftwareDisplay`), and
`BackendDescriptor` requires `make_backend` to receive the concrete `IDisplay`
its own `make_display` produced. A single key with an internal renderer switch
would have to resolve the renderer *before* `make_display` runs — re-implementing
key resolution inside a descriptor. Three keys keep the invariant.

### What the adopted-fd path does differently

The compositor, not this process, is the lessor:

- **No `drmSetMaster`, no foreground-VT guard.** The lease fd is already master
  over its leased object set; the compositor owns the VT.
- **No `drmDropMaster` at teardown.** The lease is returned by destroying the
  `wp_drm_lease_v1` object, which lets the compositor revoke and re-advertise.
- **No libseat session.** The compositor already holds the session controller for
  the seat; taking it would fight. Input falls back to direct evdev.
- **Outputs are enumerated through the lease fd**, whose resource view the kernel
  filters to the leased objects. Re-opening the card by path would both show
  connectors we do not hold *and* require permissions a leased client may not
  have — the whole point of leasing is not needing them.

The fd is **borrowed**: `drm::Device::from_fd` does not close it, and neither does
the sink. The `LeaseHold` owns it, and must outlive everything built on it. That
is why the display holds the hold.

## Revocation

Revocation is normal, not exceptional: **every VT switch away from the compositor
is a revoke**, because the compositor loses DRM master. Hot-unplug and compositor
exit do it too. It arrives as `wp_drm_lease_v1.finished` on the monitor thread,
which latches `LeaseHold::revoked()`.

What the kernel does, verified against a real lease on vkms (see
`test/unit_test/leased_drm_vkms-test`):

- The KMS objects vanish from the fd's view — every commit naming them fails.
- **The fd stays open and remains a functional render fd**: GEM allocation and
  prime export still succeed. Leases partition mode objects only.

Renderers gate on `revoked()`. The software tier drops the frame and *parks the
vsync source* rather than discarding: parking makes the engine stall (the
documented steady state), whereas handing the baton back would spin the UI and
raster threads at 100% CPU with nothing reaching a panel.

Note `wp_drm_lease_connector_v1.withdrawn` **post-grant does not revoke** — the
spec is explicit that the lease is unaffected; just destroy the offer object.

Reacquire-and-rebuild is not implemented yet; today a revoked lease means
the panel stops updating until the process is restarted.

## Status

| Tier | Registry key | Requires | State |
|---|---|---|---|
| EGL | `wayland-leased-drm-egl` | `BUILD_BACKEND_DRM_KMS_EGL` | registered |
| Software | `wayland-leased-drm-software` | `BUILD_BACKEND_SOFTWARE` + `BUILD_SOFTWARE_SINK_DRM` | registered |
| Vulkan | `wayland-leased-drm-vulkan` | `BUILD_BACKEND_DRM_KMS_VULKAN` | registered — least lease-coupled of the three: the VkDevice was never created from the KMS fd, so only scanout/modeset touches the lease |

A tier whose renderer stack is not built is **refused, not substituted**. Leasing is
the difference between driving a connector a compositor handed you and grabbing a
card outright, so falling back to an unleased backend would do the opposite of
what was configured, on hardware you may not own.

Scope today: one connector per lease, one lease per process. Multi-connector
leases and lease-per-view are deferred; the protocol supports both.

## Build

```bash
cmake -B build -G Ninja \
  -DBUILD_BACKEND_WAYLAND_LEASED_DRM=ON \
  -DBUILD_BACKEND_DRM_KMS_EGL=ON        # or: -DBUILD_BACKEND_SOFTWARE=ON
```

The configure output says which tiers are registered. A lease-only build (no
surface backends) links `wayland-client` alone — `wayland-cursor`, `xkbcommon`
and `wayland-protocols` are surface-backend dependencies this client does not
have. `drm-lease-v1.xml` is vendored under `shell/wayland/protocols/` because the
staging XML only ships with wayland-protocols >= 1.22 and the project floor is
1.20.

## Running

```bash
homescreen --backend=wayland-leased-drm-egl --lease-connector=HDMI-A-1 -b <bundle>
```

| Flag | Env | TOML | Meaning |
|---|---|---|---|
| `--backend` | — | `[view.backend] type` | `wayland-leased-drm[-vulkan\|-egl\|-software]`; the bare family name resolves vulkan → egl → software among the tiers this build registered. Name a tier explicitly to pin it |
| `--lease-connector` | `HOMESCREEN_LEASE_CONNECTOR` | `[view.backend.lease] connector` | Connector to request by name. Unset + one offer takes it; unset + several is fatal and lists them |
| `--lease-device` | `HOMESCREEN_LEASE_DEVICE` | `[view.backend.lease] device` | Which `wp_drm_lease_device_v1` when several are advertised (one per DRM node): index or node path |
| `--lease-timeout-ms` | `HOMESCREEN_LEASE_TIMEOUT_MS` | `[view.backend.lease] timeout_ms` | Bound on the whole negotiation (default 5000) |

The timeout is load-bearing, not cosmetic: the spec lets a compositor defer the
DRM fd until it regains DRM master, so without a bound a VT-switched-away
compositor would hang startup indefinitely. Every wait in the negotiation is
bounded by it, including the roundtrips.

The `drm_*` tuning knobs pass through unchanged for the egl tier, and
`IVI_SW_DRM_MODE` / format / dither for the software tier — those describe how to
drive a connector, which a lease does not change. `IVI_SW_SINK` is **not**
consulted on the leased software path: the backend key already chose the sink.

Failures are fatal by design. A leased backend that cannot get its lease has
nothing to fall back to:

```
[E] [leased-drm] could not acquire a lease: no connectors offered for leasing
```

## Tests

| Tier | Target | Needs |
|---|---|---|
| T1 — protocol, against a mock compositor | `homescreen_lease_client_ut_test_driver` | nothing (private socket in a temp `XDG_RUNTIME_DIR`) |
| T2 — kernel-real lease | `homescreen_leased_drm_vkms_ut_test_driver` | vkms + DRM master |

T1 covers the negotiation state machine end to end: grant, denial, both
revocation timings, connector/device selection and its ambiguous and not-found
variants, the deferred-enumeration deadline, and fd-leak gates (LeakSanitizer
tracks memory, not descriptors).

T2 makes a genuine lease with `drmModeCreateLease` and feeds it to the real code,
which is the only way to reach these paths on a desktop — no compositor here will
offer a connector. It **skips itself** without vkms + DRM master, so it is safe to
run anywhere.

Getting master for T2 is the awkward part. DRM grants master to the **first
opener** of a node, and a running compositor takes it: wlroots hot-adds vkms via
udev when the module loads, so `drmSetMaster` returns `EBUSY` even under root
(and `EACCES` unprivileged, which misleadingly looks like a permissions problem).
Run it from a **bare VT with no compositor**, where the test is the first opener
and gets master implicitly:

```bash
sudo modprobe vkms
# switch to a VT with no compositor running
./homescreen_leased_drm_vkms_ut_test_driver
```

Alternatively pin the compositor away from vkms (`WLR_DRM_DEVICES`) or create a
private device via `/sys/kernel/config/vkms`.
