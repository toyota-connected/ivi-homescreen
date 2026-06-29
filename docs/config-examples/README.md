# config.toml examples — backend × shell matrix

These are review samples for the nested `config.toml` schema. They surface the
assumptions about how each backend and shell maps onto the tables.

## Orthogonality

`view.backend.type` (renderer) and `view.shell.type` (Wayland compositor
protocol) are independent **for the Wayland backends**. The non-Wayland backends
(`drm-kms-egl`, `drm-kms-vulkan`, `software`) program KMS directly — there is no
compositor, so **no shell applies** and `[view.shell]` is omitted.

| backend \ shell   | agl | xdg | ivi | simple | (no shell) |
|-------------------|-----|-----|-----|--------|------------|
| wayland-egl       | ✅  | ✅  | ✅  | ✅     | —          |
| wayland-vulkan    | ✅  | ✅  | ✅  | ✅     | —          |
| drm-kms-egl       | —   | —   | —   | —      | ✅         |
| drm-kms-vulkan    | —   | —   | —   | —      | ✅         |
| software          | —   | —   | —   | —      | ✅         |

Files are named `<backend>__<shell>.toml` for the Wayland combos and
`<backend>.toml` for the KMS-direct ones.

## What differs per shell (`[view.shell]`)

- **agl** — the only shell that uses a window *role* and an *activation area*:
  `[view.shell.window].type` (BG / PANEL_*) and
  `[view.shell.window.activation_area]`. Omitting the activation area defaults it
  to the full view extent.
- **xdg** — desktop top-level. No role / no activation area. `[global].app_id`
  becomes the `xdg_toplevel` app id.
- **ivi** — surfaces are addressed by numeric id: set `surface_id` under
  `[view.shell]` (ivi-only). No role / no activation area.
- **simple** — minimal surface; no role, no activation area.
- **auto** (not shown) — bind whatever the compositor advertises; otherwise
  identical to whichever shell is selected at runtime.

## Process-level vs per-view

`[global]` (app_id, cursor, wayland_event_mask, debug_backend) and the `[sentry]`
table are **process-level** — one value for the whole process, shared by every
`[[view]]`. Geometry, shell and backend are **per-view**.

## Arguments (`[view.args]`)

Two distinct embedder sinks, split by destination:

```toml
[view.args]
engine = ['--enable-asserts']   # -> FlutterProjectArgs.command_line_argv
                                #    (engine / Dart VM switches; app_id is
                                #    prepended as argv[0])
dart   = ['--route=/home']      # -> FlutterProjectArgs.dart_entrypoint_argv
                                #    (args to Dart main(List<String>); no argv[0])
```

"VM flags" are a subset of `engine` (there is no separate VM-flags field).
Unrecognized command-line options are appended to `engine`.

## Sentry (optional, process-level)

The `[sentry]` table is optional and shared by the whole process. The DSN is
supplied via the `SENTRY_DSN` environment variable; the table only adds metadata.
Omit it entirely to disable crash reporting.

```toml
[sentry]
release = '1.0.0'
env = 'production'
tags = { board = 'dev-board' }   # [sentry.tags] table
attachments = []                  # file paths attached to reports
```

## Single vs multi view

A per-bundle file uses one `[[view]]` (a singular `[view]` is also accepted). A
master file passed with `--config` may list several `[[view]]`, each with its own
`bundle = '<dir>'`. Relative bundle paths resolve against the master file's
directory.
