# ivi-homescreen

Flutter Linux CPP Embedder

[![Documentation Status](https://readthedocs.org/projects/ivi-homescreen/badge/?version=latest)](https://ivi-homescreen.readthedocs.io/en/latest/?badge=latest)

#### Discord Server https://discord.gg/V5uWD9fvws

## Highlights

* Desktop Plugin Registry
    * Flutter Pigeon CPP compatible
    * Plugins modeled after Window CPP
    * Plugins enabled/disabled via CMake
    * Firestore first party compatible
* Desktop Texture Registry
    * Camera first party compatible
    * Video Player first party compatible
* Platform View Framework
    * AndroidView widget compatible
* Backend Support (any subset builds into one binary; the active backend is
  selected at runtime — process-wide via `--backend`, or per view via
  `[view.backend]`)
    * Wayland EGL
    * Wayland Vulkan
    * DRM/KMS EGL (direct-to-display, no compositor)
    * DRM/KMS Vulkan (zero-copy dma-buf scanout)
    * Software (CPU renderer, no GPU or display-server dependency)
    * Leased DRM, EGL / Vulkan / software (drm-lease-v1: own a connector leased
      from a compositor while it keeps the rest of the card)
* Multi-display
    * Multiple views across multiple outputs from one process
    * Per-view output binding by connector / `wl_output` name (`[view.output]`)
    * Combined-space pointer routing, per-display touch, and a single cursor
      that follows the pointer across displays
* Same source code runs on Desktop and embedded Linux image
    * Ubuntu 18+
    * Fedora 33+
    * Yocto Dunfell/Kirkstone/Scarthgap

## Plugins

ivi-homescreen plugins are located at https://github.com/toyota-connected/ivi-homescreen-plugins

There are two ways to reference this repo:

1. Clone plugins repo to root of ivi-homescreen folder
2. Set PLUGIN_DIR to repo path. -DPLUGIN_DIR=<my path>

## Logging

Logging level support

* trace
* debug
* info
* warn
* error
* critical
* off

If environmental variable IHS_LOG_LEVEL is set, it will override the default logging level of info. The logging level can also be set via the command line argument --log-level <level>.

### DLT logging

To test DLT logging on desktop use the following

Ubuntu packages

    sudo apt-get install libdlt-dev dlt-viewer dlt-daemon dlt-tools

Fedora packages

    sudo dnf install dlt-libs-devel dlt-daemon dlt-tools

### Logging with DLT

Start new terminal

    dlt-daemon

#### View DLT log output in a terminal

Start new terminal

    dlt-receive -a localhost

## Sanitizer Support

You can enable the sanitizers with SANITIZE_ADDRESS, SANITIZE_MEMORY, SANITIZE_THREAD or SANITIZE_UNDEFINED options in
your CMake configuration. You can do this by passing e.g. -DSANITIZE_ADDRESS=On on your command line.

If sanitizers are supported by your compiler, the specified targets will be built with sanitizer support. If your
compiler has no sanitizing capabilities you'll get a warning but CMake will continue processing and sanitizing will
simply just be ignored.

## Backend Support

Any subset of backends can be compiled into a single binary; they are no longer
mutually exclusive. The active backend is resolved at runtime — process-wide via
`--backend`, or per view via `[view.backend] type` in the bundle's config.toml
(CLI overrides config). A single-backend build simply registers one backend and
uses it.

| Backend | Registry key | CMake option | Notes |
|---|---|---|---|
| Wayland EGL | `wayland-egl` | `BUILD_BACKEND_WAYLAND_EGL` | GL renderer on a Wayland compositor (default ON) |
| Wayland Vulkan | `wayland-vulkan` | `BUILD_BACKEND_WAYLAND_VULKAN` | Vulkan renderer on a Wayland compositor |
| DRM/KMS EGL | `drm-kms-egl` | `BUILD_BACKEND_DRM_KMS_EGL` | Direct-to-display GL on bare KMS (no compositor) |
| DRM/KMS Vulkan | `drm-kms-vulkan` | `BUILD_BACKEND_DRM_KMS_VULKAN` | Zero-copy dma-buf scanout on bare KMS |
| Software | `software` | `BUILD_BACKEND_SOFTWARE` | CPU renderer; no GPU or display server |
| Leased DRM (EGL) | `wayland-leased-drm-egl` | `BUILD_BACKEND_WAYLAND_LEASED_DRM` + `BUILD_BACKEND_DRM_KMS_EGL` | GL on a connector leased from a compositor via drm-lease-v1 |
| Leased DRM (Vulkan) | `wayland-leased-drm-vulkan` | `BUILD_BACKEND_WAYLAND_LEASED_DRM` + `BUILD_BACKEND_DRM_KMS_VULKAN` | Zero-copy dma-buf scanout on a leased connector |
| Leased DRM (software) | `wayland-leased-drm-software` | `BUILD_BACKEND_WAYLAND_LEASED_DRM` + `BUILD_BACKEND_SOFTWARE` | CPU renderer on a leased connector |

With no explicit selection, the resolver is environment-aware: a live Wayland
session picks `wayland-egl`, otherwise `drm-kms-egl`.

The `wayland-leased-drm-*` backends acquire a connector from a running Wayland
compositor rather than opening a card, so one process can own a panel while the
compositor owns the rest of the GPU. They are never selected implicitly, and a
leased key is refused rather than substituted if unavailable — falling back to an
unleased backend would grab hardware the operator did not ask for. The bare
family name `wayland-leased-drm` picks the first available tier. Note that a
compositor implementing drm-lease-v1 is necessary but **not** sufficient: the
wlroots family only offers connectors flagged non-desktop in EDID, so an ordinary
panel is not leasable without intervention. See
[shell/backend/wayland_leased_drm/README.md](shell/backend/wayland_leased_drm/README.md).

Running a Vulkan backend requires an engine build that supports Vulkan.

## Bundle File Override Logic

If an override file is not present, it gets loaded from a default location.

### Optional override files

#### icudtl.dat

Bundle Override

    {bundle path}/data/icudtl.dat

Yocto Default

    /usr/share/flutter/icudtl.dat

Desktop Default

    /usr/local/share/flutter/icudtl.dat

#### libflutter_engine.so

Bundle Override

    {bundle path}/lib/libflutter_engine.so

Yocto/Desktop Default - https://tldp.org/HOWTO/Program-Library-HOWTO/shared-libraries.html

## Command Line Options

`-h` / `--help` prints the grouped option list. CLI flags override TOML and apply
process-wide (a single `-w` clobbers every view's width).

A bundle (`-b`) directory has this structure:

```
  Flutter Application (bundle folder)
    data/flutter_assets
    data/icudtl.dat (optional - overrides system path)
    lib/libapp.so
    lib/libflutter_engine.so (optional - overrides system path)
```

`-b` is repeatable (one view per bundle); `--config <file>` instead points at a
master TOML listing several `[[view]]` entries (each with its own `bundle`). This
opens two 1280x1024 windows running the gallery app:

```
homescreen -b $HOME/app/gallery/.desktop-homescreen -b $HOME/app/gallery/.desktop-homescreen -w 1280 --height 1024
```

Unrecognized options (and `--engine-arg`) are forwarded to the engine command
line (`command_line_argv`); `--dart-arg` passes args to the Dart entrypoint
`main(List<String>)`. The full list below is generated from the binary's `--help`
by `scripts/gen_config_reference.py` (kept in sync; do not edit by hand):

<!-- BEGIN CLI-REFERENCE (generated by scripts/gen_config_reference.py) -->

```text
Usage:
  homescreen [OPTION...]

  -h, --help        Print this help and exit
  -b, --bundle arg  Path to a bundle directory (repeatable; required unless 
                    --config is given)
      --config arg  Path to a master config.toml defining one or more [[view]] 
                    entries (each with its own 'bundle'); overrides per-bundle 
                    config.toml

 Global options:
      --app-id arg              Application id (sets [global].app_id; used by 
                                all shells)
      --xdg-shell-app-id arg    Deprecated alias for --app-id
  -t, --cursor-theme arg        Cursor theme name (e.g. DMZ-White)
  -c, --disable-cursor          Hide the pointer cursor
  -d, --debug-backend           Enable backend debug logging
      --wayland-event-mask arg  Wayland input events to mask (e.g. 
                                pointer-axis,keyboard)

 View options:
  -w, --width arg               View width in px
      --height arg              View height in px
  -o, --output-index arg        wl_output index to place the view on
  -p, --pixel-ratio arg         Device pixel ratio
  -f, --fullscreen              Start fullscreen
  -a, --accessibility-flags arg
                                Accessibility feature bit mask (decimal / 0x.. 
                                / 0..)
      --engine-arg arg          Engine / Dart VM switch -> command_line_argv 
                                (repeatable). Unrecognized options are also 
                                forwarded here.
      --dart-arg arg            Argument to the Dart entrypoint 
                                main(List<String>) -> dart_entrypoint_argv 
                                (repeatable)

 Shell options:
      --shell arg           Wayland compositor-protocol shell: 
                            auto|xdg|agl|ivi|simple (default auto)
      --window-type arg     AGL window role: BG|PANEL_*|NORMAL (AGL shell only)
      --ivi-surface-id arg  ivi-shell numeric surface id

 Backend options:
      --backend arg  Active backend: 
                     wayland-egl|wayland-vulkan|drm-kms-egl|drm-kms-vulkan|softw
                     are|wayland-leased-drm[-egl|-vulkan|-software] (default: 
                     env-aware -- a Wayland session selects wayland-egl, else 
                     drm-kms-egl). The bare name wayland-leased-drm picks the 
                     first available leased tier; a leased backend never falls 
                     back to an unleased one.

 DRM options:
      --drm-list-modes [=arg(=)]
                                List the active backend's scanout modes and 
                                exit (optional device path; defaults to 
                                --drm-device or the backend default)
      --drm-device arg          DRM device path (e.g. /dev/dri/card0)
      --drm-connector arg       DRM connector to drive (e.g. eDP-1, HDMI-A-1); 
                                default rank-picks
      --drm-mode arg            DRM mode <WxH@R> (e.g. 1920x1080@120); default 
                                = preferred mode
      --drm-rotation arg        DRM scanout rotation in degrees: 0|90|180|270 
                                (default 0)
      --drm-compositor arg      DRM compositor strategy: auto|planes|gl
      --drm-modeset arg         DRM modeset API: auto|legacy|atomic
      --drm-allow-nonblock-modeset arg
                                Allow NONBLOCK | ALLOW_MODESET atomic commits: 
                                auto|yes|no
      --drm-primary-format arg  Primary plane format: 
                                auto|xrgb8888|xbgr8888|argb8888|abgr8888|rgb565
      --drm-overlay-planes arg  Use overlay planes: auto|yes|no
      --drm-explicit-sync arg   Use IN_FENCE_FD / OUT_FENCE_PTR on commits: 
                                auto|yes|no
      --drm-async-flip arg      Use DRM_MODE_PAGE_FLIP_ASYNC on flip-only 
                                commits: auto|yes|no
      --drm-stage-cursor arg    Stage the HW cursor into the compositor commit: 
                                auto|yes|no
      --drm-no-seat             Bypass libseat: open /dev/dri + input directly 
                                and self-acquire DRM master, skipping the 
                                foreground-VT guard (headless/SSH/kiosk; you 
                                must ensure nothing else holds the display)
      --input-transform arg     Per-device pointer transform (repeatable): 
                                "<device-name-substring>=<0|90|180|270>[,flip-x]
                                [,flip-y]"

 Lease options:
      --lease-device arg      wayland-leased-drm: which wp_drm_lease_device_v1 
                              to lease from when several are advertised (one 
                              per DRM node) -- a decimal index or a node path 
                              (/dev/dri/card1). Default: the sole device; 
                              ambiguous is fatal.
      --lease-connector arg   wayland-leased-drm: connector to request by name 
                              (e.g. HDMI-A-1). Default: the sole offer; several 
                              offers with no choice is fatal.
      --lease-on-revoke arg   wayland-leased-drm: what to do when the 
                              compositor revokes the lease (it does so on every 
                              VT switch away from it, not just on error). exit 
                              (default) = log the cause and exit non-zero so a 
                              supervisor restarts and renegotiates; gate = keep 
                              running with a frozen panel (debugging only, 
                              nothing recovers). reacquire is not implemented 
                              yet.
      --lease-timeout-ms arg  wayland-leased-drm: bound on the whole lease 
                              negotiation. Default 5000. The protocol lets a 
                              compositor defer the DRM fd until it regains DRM 
                              master, so this is what stops a VT-switched-away 
                              compositor hanging startup.
      --lease-input arg       wayland-leased-drm: whether to read input from 
                              raw evdev. A leased client has no wl_surface, so 
                              libinput on /dev/input/event* is its only input 
                              source, and it is ungrabbed -- on a host with a 
                              live Wayland session that duplicates every 
                              keystroke into this process and the session 
                              compositor. auto (default) = read evdev on an 
                              embedded target but disable it when a Wayland 
                              session is detected; on = always read evdev; off 
                              = never.
```

<!-- END CLI-REFERENCE -->

## Configuration reference (all options)

The table below lists every key the parser reads. It is generated from the
parser by `scripts/gen_config_reference.py` (so it cannot drift). For runnable
per-scenario configs see [`docs/config-examples/`](docs/config-examples/); for an
annotated all-keys file see
[`docs/config-examples/reference.toml`](docs/config-examples/reference.toml).
"applies to": `agl`/`ivi` = that shell only; `drm`/`drm/sw` = those backends'
`[view.backend.drm]` table; `wayland` = Wayland backends; `all` = always.

<!-- BEGIN CONFIG-REFERENCE (generated by scripts/gen_config_reference.py) -->

| table | key | type | default | values | applies to |
|---|---|---|---|---|---|
| `[global]` | `app_id` | `string` | `homescreen` | `string` | all |
| `[global]` | `cursor_theme` | `string` | `—` | `string` | all |
| `[global]` | `debug_backend` | `bool` | `false` | `true\|false` | all |
| `[global]` | `disable_cursor` | `bool` | `false` | `true\|false` | all |
| `[global]` | `wayland_event_mask` | `string` | `—` | `comma/space list` | wayland |
| `[sentry]` | `attachments` | `string` | `[]` | `array<path>` | all |
| `[sentry]` | `env` | `string` | `(built-in)` | `string` | all |
| `[sentry]` | `release` | `string` | `—` | `string` | all |
| `[sentry]` | `tags` | `string` | `{}` | `table<string,string>` | all |
| `[[view]]` | `accessibility_features` | `int` | `0` | `int bitmask` | all |
| `[[view]]` | `bundle` | `string` | `(from -b)` | `path` | --config |
| `[[view]]` | `fullscreen` | `bool` | `false` | `true\|false` | all |
| `[[view]]` | `height` | `int` | `720` | `int` | all |
| `[[view]]` | `output_index` | `int` | `0` | `int` | all |
| `[[view]]` | `pixel_ratio` | `float` | `1.0` | `float` | all |
| `[[view]]` | `width` | `int` | `1920` | `int` | all |
| `[view.args]` | `dart` | `array<string>` | `[]` | `array<string>` | all |
| `[view.args]` | `engine` | `array<string>` | `[]` | `array<string>` | all |
| `[view.shell]` | `surface_id` | `int` | `—` | `int` | ivi |
| `[view.shell]` | `type` | `string` | `auto` | `auto\|xdg\|agl\|ivi\|simple` | all |
| `[view.shell.window]` | `type` | `string` | `NORMAL` | `BG\|PANEL_TOP\|PANEL_BOTTOM\|PANEL_LEFT\|PANEL_RIGHT\|NORMAL` | agl |
| `[view.shell.window.activation_area]` | `height` | `int` | `view.height` | `int` | agl |
| `[view.shell.window.activation_area]` | `width` | `int` | `view.width` | `int` | agl |
| `[view.shell.window.activation_area]` | `x` | `int` | `0` | `int` | agl |
| `[view.shell.window.activation_area]` | `y` | `int` | `0` | `int` | agl |
| `[view.backend]` | `type` | `string` | `(env-aware)` | `wayland-egl\|wayland-vulkan\|drm-kms-egl\|drm-kms-vulkan\|software` | all |
| `[view.backend.drm]` | `allow_nonblock_modeset` | `string` | `auto` | `auto\|yes\|no` | drm |
| `[view.backend.drm]` | `async_flip` | `string` | `auto` | `auto\|yes\|no` | drm |
| `[view.backend.drm]` | `compositor` | `string` | `auto` | `auto\|planes\|gl` | drm |
| `[view.backend.drm]` | `connector` | `string` | `(rank-pick)` | `e.g. eDP-1, HDMI-A-1` | drm/sw |
| `[view.backend.drm]` | `device` | `string` | `(rank-pick)` | `/dev/dri/by-path/<path>-card or /dev/dri/cardN` | drm/sw |
| `[view.backend.drm]` | `explicit_sync` | `string` | `auto` | `auto\|yes\|no` | drm |
| `[view.backend.drm]` | `input_transforms` | `array<string>` | `[]` | `array<string>` | drm/sw |
| `[view.backend.drm]` | `mode` | `string` | `(preferred)` | `<W>x<H>@<R>` | drm/sw |
| `[view.backend.drm]` | `modeset` | `string` | `auto` | `auto\|legacy\|atomic` | drm |
| `[view.backend.drm]` | `no_seat` | `bool` | `false` | `true\|false` | drm/sw |
| `[view.backend.drm]` | `overlay_planes` | `string` | `auto` | `auto\|yes\|no` | drm |
| `[view.backend.drm]` | `primary_format` | `string` | `auto` | `auto\|xrgb8888\|xbgr8888\|argb8888\|abgr8888\|rgb565` | drm |
| `[view.backend.drm]` | `rotation` | `int` | `0` | `0\|90\|180\|270` | drm |
| `[view.backend.drm]` | `stage_cursor` | `string` | `auto` | `auto\|yes\|no` | drm |
| `[view.backend.lease]` | `connector` | `string` | `(sole offer)` | `e.g. HDMI-A-1` | leased |
| `[view.backend.lease]` | `device` | `string` | `(sole device)` | `index or /dev/dri/by-path/<path>-card or /dev/dri/cardN` | leased |
| `[view.backend.lease]` | `input` | `string` | `auto` | `auto\|on\|off` | leased |
| `[view.backend.lease]` | `on_revoke` | `string` | `exit` | `exit\|gate` | leased |
| `[view.backend.lease]` | `timeout_ms` | `int` | `5000` | `> 0` | leased |
| `[view.output]` | `drm_connector` | `string` | `(none)` | `e.g. HDMI-A-1` | drm |
| `[view.output]` | `index` | `int` | `(none)` | `int` | all |
| `[view.output]` | `name` | `string` | `(primary)` | `e.g. DP-1, HDMI-A-1` | all |
| `[view.output]` | `on_disconnect` | `string` | `suspend` | `suspend\|teardown` | all |
| `[view.output]` | `output_id` | `string` | `(none)` | `role name from a udev rule` | drm |
| `[view.output]` | `preload` | `bool` | `false` | `true\|false` | all |
| `[view.output]` | `serial` | `string` | `(none)` | `EDID serial` | drm |
| `[view.output]` | `touch_device` | `string` | `(primary)` | `device-name substring` | all |
| `[view.output]` | `wl_name` | `string` | `(none)` | `e.g. DP-1` | wayland |
| `[view.output]` | `x` | `int` | `0` | `int (px)` | all |
| `[view.output]` | `y` | `int` | `0` | `int (px)` | all |
| `[view.engine]` | `merge_render_platform` | `bool` | `false` | `true\|false` | all |
| `[view.hud]` | `bg_alpha` | `float` | `0.75` | `float [0,1]` | all |
| `[view.hud]` | `corner` | `string` | `top-left` | `top-left\|top-right\|bottom-left\|bottom-right` | all |
| `[view.hud]` | `enable` | `bool` | `false` | `true\|false` | all |
| `[view.hud]` | `font_scale` | `float` | `1.0` | `float` | all |
| `[view.hud]` | `margin` | `float` | `12` | `float (px)` | all |
| `[view.hud]` | `text_color` | `string` | `#FFFFFF` | `#RRGGBB or #RRGGBBAA` | all |

<!-- END CONFIG-REFERENCE -->

## config.toml

Each bundle has a `config.toml` at its root (the `-b <bundle>` directory). A
master file passed with `--config <file>` may instead define several `[[view]]`
entries, each with its own `bundle`. Comments are allowed, an empty file is
valid, and CLI flags override the file.

The schema is grouped by table:

- `[global]` — process-level: `app_id`, cursor, `wayland_event_mask`,
  `debug_backend`, and the optional `[sentry]` table.
- `[[view]]` — one per view: geometry, plus
  - `[view.args]` — `engine` (→ engine/Dart VM command line) and `dart`
    (→ Dart `main()` args),
  - `[view.shell]` — `type`; for AGL, `[view.shell.window]` (role) and
    `[view.shell.window.activation_area]`; for ivi, `surface_id`,
  - `[view.backend]` — `type`; for DRM/software, the `[view.backend.drm]` knobs,
  - `[view.output]` — pin the view to a physical output by connector /
    `wl_output` name, place its display in the combined pointer space (`x`/`y`),
    and bind a touch panel (`touch_device`).

Every key (type, default, valid values, applies-to) is in the
[Configuration reference](#configuration-reference-all-options) above — that
table is generated from the parser. Runnable per-scenario files are in
[`docs/config-examples/`](docs/config-examples/); a single annotated all-keys
file is [`docs/config-examples/reference.toml`](docs/config-examples/reference.toml);
a minimal default is in [`config.toml`](config.toml).

A minimal AGL background view:

```toml
[global]
app_id = 'homescreen'

[[view]]
width = 1920
height = 1080

  [view.shell]
  type = 'agl'
    [view.shell.window]
    type = 'BG'
      [view.shell.window.activation_area]
      x = 0
      y = 64
      width = 1920
      height = 1016

  [view.backend]
  type = 'wayland-egl'
```

### Multiple displays

Drive several displays from one process by launching one view per output — a
repeated `-b <bundle>` on the command line, or several `[[view]]` entries in a
`--config` master file — each pinned to a connector with `[view.output]`. On a
DRM card with two monitors:

```toml
[global]
app_id = 'homescreen'

[[view]]
bundle = '/usr/share/gallery'
  [view.backend]
  type = 'drm-kms-egl'
    [view.backend.drm]
    # by-path rather than cardN: see "Naming a card and an output" below.
    device = '/dev/dri/by-path/platform-gpu-card'
  [view.output]
  drm_connector = 'HDMI-A-1'
  x = 0            # position in the combined pointer space

[[view]]
bundle = '/usr/share/cluster'
  [view.backend]
  type = 'drm-kms-egl'
    [view.backend.drm]
    device = '/dev/dri/by-path/platform-gpu-card'
  [view.output]
  drm_connector = 'DP-1'
  x = 1920         # placed to the right of HDMI-A-1
```

The `x`/`y` values lay the displays out in a shared pointer space so the cursor
crosses between them as arranged, and a single cursor is shown on whichever
display the pointer is over. `touch_device` binds a touch panel (by libinput
device-name substring) to the view on its display.

### Naming a card and an output

Three keys can identify hardware, and they are stable in different ways. Getting
this wrong produces a config that works on the bench and binds nothing in the
field, silently.

**The card — use the by-path name.** `/dev/dri/cardN` numbers are assigned in
probe order and are not portable. Measured on two boards running the same
software:

| driver | Raspberry Pi 4 | Raspberry Pi 5 |
| --- | --- | --- |
| `vc4-drm` | `card1` | `card0` |
| `v3d` | `card0` | `card2` |
| `drm-rp1-dsi` | — | `card1` |

`/dev/dri/by-path/<path>-card` is derived from the hardware topology instead, so
it does not move. `ls /dev/dri/by-path` to find yours. Any path is opened as
given, so this needs no other change.

**The output — use the connector name.** `drm_connector = 'HDMI-A-1'` (DRM) or
`wl_name` (Wayland) is readable, matches what the kernel, `drm_info`, Weston and
sway all print, and is unambiguous within a card. Prefer it over anything more
exotic.

Two caveats:

- *Connector names are not portable across boards either.* The DSI panel is
  `DSI-1` on a Pi 4 and `DSI-2` on a Pi 5 — same role, different name, and on a
  different card.
- *Compositors may rename.* Weston and wlroots pass the kernel name through
  (`HDMI-A-1`); Mutter reports `HDMI-1`. A `wl_name` written against a desktop
  session may not match on target.

**Matching by `serial` is a last resort, and often unavailable.** It works only
where EDID carries a unique serial, and frequently it does not: a panel wired to
a fixed port often publishes no EDID at all (both Raspberry Pi DSI panels report
zero bytes), and two identical monitors commonly share one. When a serial is
ambiguous the resolver says so and falls back to the connector name.

**For a config that must serve more than one board, name the role.** Every key
above is stable per board and none survives a hardware change: the connector
name moves, the card number moves, and a panel with no EDID has no serial to
match on. Nothing the hardware reports names the *role* a display plays.

A udev rule can. udev models DRM connectors as devices (`DEVTYPE=drm_connector`),
so a rule can attach a property to one, and `[view.output] output_id` matches on
it:

```toml
[view.output]
output_id = 'cluster'
```

```
# /etc/udev/rules.d/99-ihs-outputs.rules
ENV{ID_PATH}=="platform-gpu", KERNEL=="card*-DSI-1", ENV{IHS_OUTPUT_NAME}="cluster"
```

One config then binds correctly on both boards, which is the whole point: the
DSI panel is `DSI-1` on a Pi 4 and `DSI-2` on a Pi 5, and `output_id = 'cluster'`
reaches it on either. A worked example covering both boards ships as
[`99-ihs-outputs.rules`](docs/config-examples/99-ihs-outputs.rules).

Two things to know when writing the rule:

- `DEVTYPE` is a property, not a match key of its own — it must be written
  `ENV{DEVTYPE}=="drm_connector"`, and `udevadm verify` rejects the other form.
- `ID_PATH` is per GPU, **not** per connector. On a Pi 4 all three connectors
  share `platform-gpu`, so `ID_PATH` alone cannot identify one; pair it with a
  `KERNEL` pattern as above.

`output_id` is matched *above* `serial`, and unlike the other keys a miss parks
the view rather than falling through to a lower tier — naming a role is a
deliberate statement about which physical display a view belongs on, and
quietly binding an instrument cluster to the passenger screen because the rule
was missing is not a graceful degradation. Check the log if a view does not
appear:

```
[OutputResolver] output_id 'cluster' matches no connected output; the view is parked
```

**DRM only for now.** `DrmOutputProvider` fills `output_id`; the Wayland
`Display` does not, because a `wl_output` carries no connector identity a client
can join on — and the name a compositor advertises need not be the kernel's (see
the Mutter caveat above). An `output_id` match on Wayland therefore finds
nothing and parks, which the log reports.

### When an output disappears

Both backends report connector hotplug: Wayland through `wl_output`
add/remove/`done`, DRM through a udev connector monitor. On each change every
view on that display is re-resolved and compared with the output it is actually
on, and `[view.output] on_disconnect` decides what happens to a view whose
output has gone:

- `suspend` (the default) parks the view. Its platform views are told to stop
  producing — a camera or video plugin should pause its decode there — and
  frame production stops by withholding the vsync baton, so the UI thread,
  rasterizer, GPU and scanout all go quiet. The engine and the surface are kept,
  so returning is immediate: the view resumes when its output comes back.
- `teardown` is **not implemented yet**. It warns and suspends instead, rather
  than appearing to free the engine.

`preload` is likewise parsed but not acted on yet: a view is built either way.

Moving a running view onto a *different* output is also not implemented — the
transition is detected, logged, and the view's platform views are told their
grant is stale, but the view is not rebound.

> Breaking change vs. earlier releases: the flat `[window_activation_area]`
> table, `view.window_type`, `view.shell`/`view.backend` strings, `view.vm_args`,
> and the `view.drm_*`/`view.ivi_surface_id` keys were replaced by the nested
> tables above (`[view.shell.window.activation_area]`, `[view.shell].type`,
> `[view.backend].type`, `[view.args].engine`/`dart`, `[view.backend.drm].*`,
> `[view.shell].surface_id`).

## Parameter loading order

Resolved per view; later layers override earlier ones key-by-key. Only
`[view.args]` (engine/dart) and unmatched CLI options are additive (appended):

1. built-in defaults
2. the bundle's own `<bundle>/config.toml`
3. the `--config` master file's matching `[[view]]` entry (when given)
4. command-line flags — override everything, applied process-wide

## CMake Build flags

`ENABLE_XDG_CLIENT` - Enable XDG Client. Defaults to ON

`ENABLE_AGL_SHELL_CLIENT` - Enable AGL Client. Defaults to OFF

`ENABLE_IVI_SHELL_CLIENT` - Enable ivi-shell Client. Defaults to OFF

`ENABLE_DRM_LEASE_CLIENT` - Enable drm lease Client. Defaults to OFF

`ENABLE_LTO` - Enable Link Time Optimization. Defaults to OFF

`ENABLE_DLT` - Enable DLT logging. Defaults to OFF

`BUILD_BACKEND_WAYLAND_EGL` - Build Backend for EGL. Defaults to ON

`BUILD_EGL_TRANSPARENCY` - Build with EGL Transparency Enabled. Defaults to ON

`BUILD_EGL_ENABLE_3D` - Build with EGL Stencil, Depth, and Stencil config Enabled. Defaults to ON

`BUILD_EGL_ENABLE_MULTISAMPLE` - Build with EGL Sample set to 4. Defaults to ON

`BUILD_BACKEND_WAYLAND_VULKAN` - Build Backed for Vulkan. Defaults to OFF

`BUILD_COMPOSITOR` - Enable the `FlutterCompositor` backing-store API so platform-view layers can be interleaved with Flutter UI. See [Platform View Plugins](#platform-view-plugins). Defaults to OFF.

`BUILD_COMPOSITOR_DMABUF_EXPORT` - When the Vulkan backend is active and `BUILD_COMPOSITOR=ON`, export each `VulkanBackingStore`'s memory as a DMA-BUF fd so plugins can import it zero-copy (requires `VK_KHR_external_memory_fd` at runtime; silently falls back to a plain allocation if unavailable). Defaults to OFF.

`DEBUG_PLATFORM_MESSAGES` - Dump Platform Channel Messages. Defaults to OFF

`BUILD_CRASH_HANDLER` - Build Sentry IO Crash Handler Support. Defaults to OFF

`BUILD_DOCS` - Builds Docs. Defaults to OFF

`BUILD_UNIT_TESTS` - Build Unit Tests. Defaults to OFF

`UNIT_TEST_SAVE_GOLDENS` - Update test goldens. Defaults to OFF

`EXE_OUTPUT_NAME` - Set executable output name. Defaults to `homescreen`

`DISABLE_PLUGINS` - Disables all plugins located in the plugins folder. Defaults to OFF

`BUILD_PLUGIN_AUDIOPLAYERS_LINUX` - Include Audioplayers Linux plugin. Defaults to OFF

`BUILD_PLUGIN_CAMERA` - Include Camera plugin. Defaults to OFF

`BUILD_PLUGIN_CLOUD_FIRESTORE` - Plugin Cloud Firestore. Defaults to OFF

`BUILD_PLUGIN_DESKTOP_WINDOW_LINUX` - Includes Desktop Window Linux Plugin. Defaults to OFF

`BUILD_PLUGIN_FILE_SELECTOR` - Include File Selector plugin. Defaults to OFF

`BUILD_PLUGIN_FIREBASE_AUTH` - Plugin Firebase Auth. Defaults to OFF

`BUILD_PLUGIN_FIREBASE_STORAGE` - Plugin Firebase Storage. Defaults to OFF

`BUILD_PLUGIN_GO_ROUTER` - Includes Go Router Plugin. Defaults to ON

`BUILD_PLUGIN_GOOGLE_SIGN_IN` - Include Google Sign In manager. Defaults to OFF

`BUILD_PLUGIN_INTEGRATION_TEST` - Included Flutter Integration Test support. Defaults to OFF

`BUILD_PLUGIN_PDF` - Include PDF plugin. Defaults to OFF

`BUILD_PLUGIN_SECURE_STORAGE` - Includes Flutter Secure Storage. Defaults to OFF

`BUILD_PLUGIN_URL_LAUNCHER` - Includes URL Launcher Plugin. Defaults to OFF

`BUILD_PLUGIN_VIDEO_PLAYER_LINUX` - Include Video Player plugin. Defaults to OFF

`BUILD_PLUGIN_FILAMENT_VIEW` - Include Filament View plugin. Defaults to OFF

`BUILD_PLUGIN_LAYER_PLAYGROUND_VIEW` - Include Layer Playground View plugin. Defaults to OFF

`BUILD_PLUGIN_NAV_RENDER_VIEW` - Include Navigation Render View plugin. Defaults to OFF

`BUILD_PLUGIN_WEBIVEW_FLUTTER_VIEW` - Includes WebView View Plugin. Defaults to OFF

`BUILD_WATCHDOG` - Build Watchdog support. Monitors main and render threads for hangs and aborts on timeout. Defaults to OFF

`BUILD_SYSTEMD_WATCHDOG` - Integrate with systemd watchdog (sd_notify). Requires `BUILD_WATCHDOG=ON` and a systemd-enabled Linux distro. Defaults to OFF

Each `BUILD_BACKEND_*` option gates whether that backend is compiled in; any
subset may be enabled together and the active one is chosen at runtime (see
[Backend Support](#backend-support)).

## Platform View Plugins

When `-DBUILD_COMPOSITOR=ON`, the EGL and Vulkan backends wire the `FlutterCompositor` backing-store API so that a plugin-owned native surface can be interleaved between Flutter-rendered layers — what the Flutter framework calls a `PlatformViewLayer`. Without this flag the engine runs in single-surface mode and platform views fall back to full-screen overlays.

A plugin participates by implementing `ICompositorSurface` (`shell/view/compositor_surface_interface.h`) and registering the instance with `FlutterView` when the Dart widget is created:

```cpp
#include "view/compositor_surface_interface.h"
#include "view/flutter_view.h"

class MyPlatformView : public ICompositorSurface {
 public:
  explicit MyPlatformView(FlutterPlatformViewIdentifier id) : id_(id) {}

  bool OnCreateBackingStore(const FlutterBackingStoreConfig* config,
                            FlutterBackingStore* out) override {
    // Fill `out` with a backing store your plugin renders into. Most
    // plugins delegate to the engine-provided backing store and only
    // override if they need a specific image/format (e.g. DMA-BUF).
    return false;
  }
  bool OnCollectBackingStore(const FlutterBackingStore*) override { return true; }
  bool OnPresent(const FlutterLayer* layer) override {
    // Draw / swap your native surface here. The compositor has already
    // reconciled the Wayland subsurface Z-order before this call.
    return true;
  }
  FlutterPlatformViewIdentifier GetIdentifier() const override { return id_; }
  void OnResize(int32_t w, int32_t h) override { /* re-size native surface */ }

 private:
  FlutterPlatformViewIdentifier id_;
};

// On the flutter/platform_views "create" message:
flutter_view->RegisterCompositorSurface(
    id, std::make_shared<MyPlatformView>(id));

// On "dispose":
flutter_view->UnregisterCompositorSurface(id);
```

`PlatformViewsHandler` calls `UnregisterCompositorSurface` automatically on `dispose` and routes `resize` messages to `ICompositorSurface::OnResize`, so plugins only need to register on create.

Key details:

- `OnPresent` runs on the engine's rasterizer thread. If your plugin renders on its own thread, marshal work there.
- The Vulkan backend hands the engine a `VkImage` in `VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL`. The compositor records all layout transitions — plugins that *consume* the image via `ICompositorSurface::OnPresent` should be prepared to sample in `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` (compositor transitions it to `TRANSFER_SRC_OPTIMAL` during the blit; see `WaylandVulkanBackend::BlitStoreToSwapchain`).
- With `-DBUILD_COMPOSITOR_DMABUF_EXPORT=ON`, a plugin can query `WaylandVulkanBackend::HasDmaBufExport()` to know whether the store exposes a DMA-BUF fd for zero-copy import into EGL/KMS/Filament.
- On pure GLES2 drivers, the EGL compositor falls back to a textured-quad program instead of `glBlitFramebuffer`; no plugin action required.

Compositor mode is opt-in. The default (`BUILD_COMPOSITOR=OFF`) build is unchanged, so existing plugins that draw via a legacy `wl_subsurface` path keep working as before.

## x86_64 Desktop development notes

### NVidia GL errors

Running EGL backend on a Lenovo Thinkpad with NVidia drivers may generate many GL runtime errors.
This should resolve it:

```
export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
```

## Ubuntu 16-18

### Logging in

Log out if logged in Login screen
Click on username field
Right-click on the gear icon below username field, and select "Ubuntu on Wayland"
Enter password and login

## Ubuntu 20+ / Fedora 33+

Defaults to Wayland, no need to do anything special

## Build steps

### Required Packages

    sudo add-apt-repository ppa:kisak/kisak-mesa
    sudo apt-get update -y
    sudo apt-get -y install libwayland-dev wayland-protocols \
    mesa-common-dev libegl1-mesa-dev libgles2-mesa-dev mesa-utils \
    libxkbcommon-dev

### Optional Packages

    # To build doxygen documentation
    sudo apt-get -y install doxygen

### GCC/libstdc++ Build

Without plugins

    git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git
    mkdir build && cd build
    cmake ../ivi-homescreen -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
    make install -j

With plugins

    git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git
    git clone https://github.com/toyota-connected/ivi-homescreen-plugins.git
    mkdir build && cd build
    cmake ../ivi-homescreen -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local -DPLUGINS_DIR=`pwd`/ivi-homescreen-plugins
    make install -j

### Clang/libc++ Build

Without plugins

    git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git
    mkdir build && cd build
    CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake ../ivi-homescreen -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local
    make install -j

With plugins

    git clone --recurse-submodules -j8 https://github.com/toyota-connected/ivi-homescreen.git
    git clone https://github.com/toyota-connected/ivi-homescreen-plugins.git
    mkdir build && cd build
    CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake ../ivi-homescreen -DCMAKE_STAGING_PREFIX=`pwd`/out/usr/local -DPLUGINS_DIR=`pwd`/ivi-homescreen-plugins
    make install -j

#### Clang Toolchain Setup

    wget https://apt.llvm.org/llvm.sh
    chmod +x llvm.sh
    sudo ./llvm.sh 14
    sudo apt-get install -y libc++-14-dev libc++abi-14-dev libunwind-dev

## CI Example

    https://github.com/toyota-connected-na/ivi-homescreen/blob/main/.github/workflows/ivi-homescreen-linux.yml

## Debian Package

    make package -j
    sudo apt install ./ivi-homescreen-1.0.0-Release-beta-Linux-x86_64.deb

## Flutter Application

### Running an app

Release Bundle Folder layout

```
.desktop-homescreen/
├── data
│ ├── flutter_assets
│ │   └── ... 
│ └── icudtl.dat
├── default_config.json (optional)
└── lib
    ├── libapp.so
    └── libflutter_engine.so
```

Running the bundle above would be

```
homescreen --b=`pwd`/.desktop-homescreen --w=1024 --h=768
```

## workspace-automation provides a flutter workspace setup tool

https://github.com/meta-flutter/workspace-automation

Example usage to run gallery application on Linux desktop

Run once

```
git clone https://github.com/meta-flutter/workspace-automation
cd workspace_automation
sudo ./flutter_workspace.py
```

Run for each development session, or new terminal window opened

```
source ./setup_env.sh
cd app/gallery
flutter run -d desktop-homescreen
```

flutter_workspace.py installs runtime packages, patches source files, compiles projects, etc.

_Note: `sudo` is required to install runtime packages_

## CMAKE dependency paths

Path prefix used to determine required files is determined at build.

For desktop `CMAKE_INSTALL_PREFIX` defaults to `/usr/local`
For target Yocto builds `CMAKE_INSTALL_PREFIX` defaults to `/usr`

## Watchdog

The watchdog monitors the main thread and Flutter render thread for hangs. If either thread fails to check in within the timeout window (default 5 seconds), the process calls `abort()` to generate a core dump — or triggers `sd_notify(WATCHDOG=trigger)` when built with systemd support.

### CMake Variables

To enable watchdog support:

    -DBUILD_WATCHDOG=ON

To additionally integrate with the systemd service watchdog:

    -DBUILD_WATCHDOG=ON -DBUILD_SYSTEMD_WATCHDOG=ON

With systemd integration enabled, the embedder reads the `WatchdogSec=` interval from the service unit, sends `READY=1` on startup, `WATCHDOG=1` each ping, and `STOPPING=1` on clean shutdown.

### Watchdog sources

The watchdog tracks named integer source IDs (`WatchdogSource`, a `typedef int64_t`). The built-in sources are:

| Constant | Value | Thread monitored |
|---|---|---|
| `WATCHDOG_SOURCE_MAIN_THREAD` | 0 | Application main loop |
| `WATCHDOG_SOURCE_RENDER_THREAD` | 1 | Flutter rasterizer thread |

Source IDs 3–255 are available for Dart-side registration via the platform channel.

#### Source name and timeout configuration

The `[watchdog]` table in `config.toml` is optional and accepts:

| Key | Type | Default | Description |
|---|---|---|---|
| `timeout_ms` | integer | `5000` | Watchdog timeout in milliseconds. Ignored when `BUILD_SYSTEMD_WATCHDOG=ON` (the `WatchdogSec=` unit interval takes precedence). |
| `source_names` | table | — | Maps source IDs (decimal string keys) to human-readable names used in log output. |

```toml
[watchdog]
timeout_ms = 10000

[watchdog.source_names]
1 = 'MyFlutterApp'
2 = 'BackgroundSync'
```

Source names can also be set or overridden at runtime by passing the optional `name` argument to the `start` platform channel method. The runtime value takes precedence over any config-defined name.


### Platform channel

When `BUILD_WATCHDOG=ON`, a `"watchdog"` platform channel is registered and available to Flutter apps via `StandardMethodCodec`. Methods:

| Method | Arguments | Description |
|---|---|---|
| `get_callbacks` | — | Returns a map of `start`, `pet`, `stop` native function pointers (FFI callable from Dart) |
| `start` | `{"source": int64, "name": string?}` | Register and begin monitoring source ID; optional `name` overrides any config-defined name for logging |
| `pet` | `{"source": int64}` | Reset the timeout for source ID |
| `stop` | `{"source": int64}` | Deregister source ID |

Source IDs passed from Dart must be non-negative. There is no upper-bound restriction.


### Example (Dart)

Use `get_callbacks` to retrieve native function pointers and call them directly from Dart FFI for zero-overhead petting on hot paths:

```dart
import 'dart:ffi';
import 'package:ffi/ffi.dart';

// Use method channels to get callbacks from the native watchdog implementation
NativeFunction<Void Function(Int64)>>? startCallback;
NativeFunction<Void Function(Int64)>>? petCallback;
NativeFunction<Void Function(Int64)>>? stopCallback;

void initWatchdog() async {
  final channel = MethodChannel('watchdog');
  final callbacks = await channel.invokeMethod<Map>('get_callbacks');

  final startCallbackPtr = Pointer.fromAddress(callbacks['start']);
  startCallback = startCallbackPtr.asFunction<void Function(int64)>();
  final petCallbackPtr = Pointer.fromAddress(callbacks['pet']);
  petCallback = petCallbackPtr.asFunction<void Function(int64)>();
  final stopCallbackPtr = Pointer.fromAddress(callbacks['stop']);
  stopCallback = stopCallbackPtr.asFunction<void Function(int64)>();
}

// example usage
const int WATCHDOG_SOURCE_APP = 123;

void main() {
  initWatchdog();
  // Start monitoring the main thread
  startCallback?.call(WATCHDOG_SOURCE_APP);
  // In your main loop, periodically pet the watchdog
  petCallback?.call(WATCHDOG_SOURCE_APP);
  // On shutdown, stop monitoring
  stopCallback?.call(WATCHDOG_SOURCE_APP);
}
```

## Crash Handler

Sentry-native support is available for Crash Handling. This pushes a mini-dump to the cloud for triage and tracking.

> To create user account and get a DSN
> see https://sentry.io/welcome/

### Configuration

1. Specify your DSN via environment variable `SENTRY_DSN`

2. Add the following sections to your `config.toml` file with the following structure:

```toml
[sentry]
release = "homescreen-1.0.0"
env = "production"
attachments = [
    "/path/to/crash.log",
    "/path/to/config.toml"
]

[sentry.tags]
platform = "linux"
device = "ivi"
```

### CMake Variables

To enable crash handler support:

    -DBUILD_CRASH_HANDLER=ON
    -DSENTRY_NATIVE_LIBDIR="directory where sentry native is installed, will look in CMAKE_INSTALL_PREFIX directory if not defined"
    -DCRASHPAD_BINARY_DIR="directory where crashpad_handler executable is installed, will look in CMAKE_INSTALL_PREFIX directory if not defined"

To resolve crash dump stack trace, debug binaries and symbols need to be uploaded to Sentry via sentry-cli tool: https://docs.sentry.io/cli/installation/

Required source repo:  https://github.com/getsentry/sentry-native

### Example Build steps

sentry build

```bash
git clone https://github.com/getsentry/sentry-native
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_STAGING_PREFIX=`pwd`/out/usr
make install
```

ivi-homescreen build

```bash
git clone https://github.com/toyota-connected/ivi-homescreen
mkdir build && cd build
cmake .. -DBUILD_CRASH_HANDLER=ON -DSENTRY_NATIVE_LIBDIR=`pwd`/../sentry-native/build/out/usr/lib -DCRASHPAD_BINARY_DIR=`pwd`/../sentry-native/build/out/usr/bin
make -j
SENTRY_DSN=<your DSN> homescreen --b=<your bundle folder> --f
```

## Yocto recipes

### Scarthgap

    https://github.com/meta-flutter/meta-flutter/tree/scarthgap/recipes-graphics/toyota

### Kirkstone

    https://github.com/meta-flutter/meta-flutter/tree/kirkstone/recipes-graphics/toyota

### Dunfell

    https://github.com/meta-flutter/meta-flutter/tree/dunfell/recipes-graphics/toyota
