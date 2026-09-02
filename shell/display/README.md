# display/

The display abstraction layer for the Flutter embedder. An `IDisplay` is the
event / output source a view runs against — either a Wayland compositor
connection or a DRM card — and is the seam that makes "one master domain, N
named outputs" uniform across backends, which is what lets a single process
drive multiple physical displays from one configuration.

It also hosts the backend-agnostic output binding machinery (`OutputMatch`,
`OutputManager`) and the per-card DRM plumbing (master acquisition, libseat
session, hotplug monitoring, page-flip event routing, EDID/identity helpers,
scale math).

---

## Features

| Capability | Status | Toggle / scope |
|---|---|---|
| `IDisplay` abstraction (event pump + output provider) | ✓ | Always compiled |
| `SoftwareDisplay` — no-op display for the software backend | ✓ | `-DBUILD_BACKEND_SOFTWARE=ON`, `-DBUILD_BACKEND_HEADLESS_EGL=ON`, or `-DBUILD_BACKEND_HEADLESS_VULKAN=ON` |
| `DrmDisplay` — DRM master + libseat session + hotplug monitor | ✓ | `-DBUILD_BACKEND_DRM_KMS_EGL=ON` or `-DBUILD_BACKEND_DRM_KMS_VULKAN=ON` |
| `DrmDisplay` adopted-fd constructor (wayland-leased-drm) | ✓ | `-DBUILD_BACKEND_WAYLAND_LEASED_DRM=ON` |
| `DrmOutputProvider` — enumerates card connectors as outputs | ✓ | DRM backends |
| `OutputManager` — resolves `[view.output]` to a connector / `wl_output` | ✓ | Always compiled |
| `OutputMatch` priority tiers (output_id, EDID serial, name, index) | ✓ | Always compiled |
| EDID read (`ProbeConnectorInfo`) | ✓ | DRM backends |
| `IHS_OUTPUT_NAME` udev role names (`ReadOutputIds`) | ✓ | DRM backends |
| `ResolveDrmDevice` — picks the right `/dev/dri/cardN` | ✓ | DRM backends |
| `PrintDrmModes` for `--drm-list-modes` | ✓ | DRM backends |
| Per-card `PAGE_FLIP_EVENT` reader + dispatch routing | ✓ | DRM backends |
| `ScalePolicy` — fractional / HiDPI buffer scaling | ✓ | Always compiled (header-only) |

---

## Architecture

```mermaid
flowchart TD
    App["App<br/>(owns displays)"] --> ID["IDisplay (abstract)"]

    ID --> Wl["Wayland Display<br/>(shell/wayland)"]
    ID --> SD["SoftwareDisplay<br/>no-op"]
    ID --> DD["DrmDisplay"]

    DD --> DS["DrmSession<br/>(libseat)"]
    DD --> HPM["udev HotplugMonitor"]
    DD --> FPR["PAGE_FLIP reader thread<br/>(per card)"]

    ID -.->|GetOutputProvider()| OP["IOutputProvider"]
    OP --> DOP["DrmOutputProvider"]
    OP --> WOP["Wayland registry<br/>(shell/wayland)"]

    OP -->|EnumerateOutputs()| OM["OutputManager<br/>ResolveForView()"]
    OM -->|name| BE["Backend<br/>(scans out)"]
```

`IDisplay` is the single interface `App` talks to. A Wayland connection, a DRM
card, and the no-op software display are all the same shape to `App::Run` — it
just calls `StartEvents` / `PollEvents` / `GetRefreshRate` /
`GetOutputProvider`. The backends then reach into the concrete display to
drive their scanout.

The output-binding path is independent of any concrete display:
`OutputManager::ResolveForView()` asks the display's `IOutputProvider` for the
currently-connected outputs, runs the view's `[view.output]` constraint through
`ResolveOutput()`, and hands the connector / `wl_output` name to the backend.

### Module responsibilities

- **`IDisplay`** ([idisplay.h](idisplay.h)) — the abstract base. Event pump
  (`StartEvents` / `StopEvents` / `PollEvents`), refresh-rate and mode queries,
  `SetViewControllerState` so the seat can deliver input, and
  `GetOutputProvider` for the output-binding path.
- **`SoftwareDisplay`** ([software_display.{h,cc}](software_display.h)) —
  no-op `IDisplay` for the software backend. Refresh rate is the only field
  `App::Loop` consumes; it also owns the libinput-backed `SoftwareSeat` and,
  for the leased-DRM path, the `LeasedScanout` descriptor.
- **`DrmDisplay`** ([drm_display.{h,cc}](drm_display.h)) — the DRM card's
  master domain. Owns the libseat session (or the legacy direct-open +
  `drmSetMaster` fallback with the foreground-VT guard), the udev hotplug
  monitor, the per-card `PAGE_FLIP_EVENT` reader thread, and the plane
  coordinator. The adopted-fd constructor wires it up for wayland-leased-drm
  (no `drmSetMaster`, no libseat session, enumeration through the lease fd).
- **`IOutputProvider`** ([output_provider.h](output_provider.h)) — the
  "source of physical outputs" seam. `EnumerateOutputs()` returns the
  connected ones plus (on DRM) enumerated-but-disconnected ports;
  `SetOutputListener` installs a listener for hotplug callbacks.
- **`DrmOutputProvider`** ([drm_output_provider.{h,cc}](drm_output_provider.h))
  — enumerates the card's connectors as `OutputInfo`. Uses libdrm only (no DRM
  master, no GL/GBM), so it is safe to call before, and independently of, the
  backend. `SetEnumerationFd` switches enumeration through a lease fd.
- **`OutputInfo` / `OutputMatch` / `ResolveOutput`**
  ([output.{h,cc}](output.h)) — the data + match logic. Match tiers, most
  specific first: `output_id` (udev role), EDID `serial`, connector / `wl_name`,
  `index` (deprecated). `ResolveOutputDetailed` distinguishes
  `kUnconstrained` (no `[view.output]`) from `kUnresolved` (a constraint that
  was set and nothing satisfied it).
- **`OutputManager`** ([output_manager.{h,cc}](output_manager.h)) — the
  backend-agnostic entry point. Reads the display's provider, runs the
  resolver, logs the decision.
- **`DiffOutputs` / `ClassifyOutputTransition`**
  ([output.{h,cc}](output.h)) — pure functions that turn two enumerations into
  a list of changes, and a change + view state into an `OutputTransition`
  (`kNone` / `kMoved` / `kAppeared` / `kLost` / `kReconfigured`).
- **`ProbeConnectorInfo`** ([connector_edid.{h,cc}](connector_edid.h)) —
  reads and parses a connector's EDID. Returns nullopt when the connector has
  no EDID (ordinary on embedded DSI panels).
- **`ReadOutputIds`** ([output_identity.{h,cc}](output_identity.h)) — reads
  the `IHS_OUTPUT_NAME` udev property of each connector on a card. This is the
  only output-binding key that survives a change of board.
- **`ResolveDrmDevice`** ([drm_device_resolver.{h,cc}](drm_device_resolver.h))
  — picks which `/dev/dri/cardN` to use. Explicit → single card → first card
  with a connected non-virtual connector → refuse to guess.
- **`PrintDrmModes`** ([drm_mode_list.{h,cc}](drm_mode_list.h)) — supports
  `--drm-list-modes`; read-only, prints every connector's modes + plane
  rotation capabilities.
- **`ScalePolicy`** ([scale_policy.h](scale_policy.h)) — the normative
  fractional-scale rounding. Carries scale as `scale_120` (unity = 120),
  rounds in the integer domain, and is bit-identical to the
  wayland-cxx-scanner reference via shared conformance vectors.
- **Cursor sinks** ([icursor_shape_sink.h](icursor_shape_sink.h),
  [../input/cursor_position_sink.h](../input/cursor_position_sink.h)) — small
  interfaces so the seat can retarget a live cursor regardless of which
  concrete cursor (HW plane, GL-composited, software) the backend is using.

---

## Build steps

### Configure + build

These sources build only when the corresponding backend is enabled:

| Source | Gate |
|--------|------|
| `display/output.{h,cc}`, `display/output_manager.{h,cc}` | Always |
| `display/software_display.{h,cc}` | `BUILD_BACKEND_SOFTWARE`, `BUILD_BACKEND_HEADLESS_EGL`, or `BUILD_BACKEND_HEADLESS_VULKAN` |
| `display/drm_display.{h,cc}`, `display/connector_edid.{h,cc}`, `display/drm_device_resolver.{h,cc}`, `display/drm_mode_list.{h,cc}`, `display/output_identity.{h,cc}`, `display/drm_output_provider.{h,cc}` | `BUILD_BACKEND_DRM_KMS_EGL` or `BUILD_BACKEND_DRM_KMS_VULKAN` |

Example:

```bash
cmake -GNinja -B build -DBUILD_BACKEND_DRM_KMS_EGL=ON
ninja -C build
```

### Dependencies

- **libdrm** — connector enumeration, EDID read, mode listing.
- **drm-cxx** — `drm::Device`, `drm::session::Seat`, `HotplugMonitor`, cursor
  loading.
- **libseat** (optional) — DRM master + input fd revocation. Without it the
  display falls back to direct `/dev/dri` open + the legacy VT / master
  guards.
- **libudev** — hotplug monitoring and `IHS_OUTPUT_NAME` role names.

---

## Running

### DRM card selection

When `[view.backend.drm.device]` is not set, `ResolveDrmDevice` applies this
precedence:

1. An explicit `--drm-device` / `HOMESCREEN_DRM_DEVICE` is used verbatim.
2. Exactly one `/dev/dri/cardN` present → that card, whatever its number.
3. Several cards → the first with a connected, non-virtual connector.
4. Otherwise → **refuse to guess**; the caller fail-fasts.

### Output binding

A view pins itself to an output via the `[view.output]` table. The most
specific set field wins:

| Tier | Key | Scope | Notes |
|------|-----|-------|-------|
| 1 | `output_id` | DRM only | udev role name (`IHS_OUTPUT_NAME`). Portable across boards. |
| 2 | `serial` | DRM only | EDID serial. Only if it identifies exactly one connected output. |
| 3 | `drm_connector` / `wl_name` | Per backend | Connector / `wl_output` name. |
| 4 | `index` | All | Deprecated, unstable. |

An `output_id` that matches more than one output **parks the view** rather than
falling through — naming a role is a deliberate statement about which display
a view belongs on.

### Multi-display layout

Multiple views can share one input seat (e.g. several displays on one DRM
card). Each view declares its position in the combined pointer space via
`[view.output] x` / `y`, and binds its touch panel via `touch_device` (a
libinput device-name substring).

### Reading logs

Key prefixes:

- `[OutputManager]` — binding decisions.
- `[OutputResolver]` — resolver-tier messages (ambiguous / no-match).
- `[DrmDisplay]` — session open, VT guard, master acquisition.
- `[DrmOutputProvider]` — enumeration diagnostics.

---

## Diagnostics/Debug

- **`--drm-list-modes`** — prints every connector's modes + plane rotation
  capabilities for the resolved card. Read-only; returns before booting the
  engine.
- **`HOMESCREEN_DRM_NO_SEAT=1`** — bypass libseat; the display opens
  `/dev/dri` directly and self-acquires DRM master. Useful when debugging the
  master-acquisition path without a logind / seatd running.
- **`IHS_OUTPUT_NAME` udev rule** — to debug role-name binding, verify that the
  udev environment property is set on the connector:
  `udevadm info --query=property --path=/sys/class/drm/<card>-<connector>`.
  Note: this is a udev *environment property*, not a sysfs attribute.

---

## Known limitations

- `IHS_OUTPUT_NAME` is DRM-only today. The Wayland path cannot read a
  connector identity, so a compositor advertising `HDMI-1` for a kernel
  `HDMI-A-1` will not resolve by name without a normalization step.
- `OutputMatch::index` is deprecated. Enumeration order can shift across
  probes and reboots; the resolver logs a warning when it is used.
- A `DrmDisplay` built through the adopted-fd path has no
  `no_seat` knob — the compositor owns the VT, so the answer is always "no
  seat".

---

## References

- [`shell/display/idisplay.h`](idisplay.h) — `IDisplay` interface
- [`shell/display/drm_display.h`](drm_display.h) — `DrmDisplay` + adopted-fd
  constructor
- [`shell/display/output.h`](output.h) — `OutputInfo`, `OutputMatch`,
  `ResolveOutput`, `DiffOutputs`
- [`shell/display/output_manager.h`](output_manager.h) — `OutputManager`
- [`shell/configuration/README.md`](../configuration/README.md) — `[view.output]`
  config reference
- [`shell/backend/drm_kms_egl/README.md`](../backend/drm_kms_egl/README.md) —
  the DRM/KMS-EGL backend that consumes `DrmDisplay`
- [`shell/backend/software/README.md`](../backend/software/README.md) — the
  software backend that consumes `SoftwareDisplay`
