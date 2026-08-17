# mcp_drive_test

Integration test app for **driving ivi-homescreen over MCP**, plus a driver
that acts through the socket and checks what the application actually did.

Every other MCP test in this repo runs against a mock host. Those prove a
request reaches the shell; they cannot prove Flutter did anything with it,
because there is no Flutter on the other side. This fixture is that other
half:

```
MCP client (tools/drive.py)
  -> Unix socket -> HTTP -> JSON-RPC        (transport)
  -> ihs_mcp_registry -> semantics provider (routing)
  -> ihs_semantics_dispatch                 (funnel: mask, attribution)
  -> FlutterEngineSendSemanticsAction       (embedder)
  -> SemanticsOwner.performAction           (framework)
  -> the closure the widget registered      <- only this fixture reaches here
```

## The app reports through MCP

Every handler writes into a status line that is itself a semantics node, so
the driver reads results back over the same surface it used to act. The read
path is therefore the assertion channel for the write path, and neither can
pass on its own.

The status value is semicolon-separated `key=value`, because one field is
text the driver chose and the realistic `ui_set_text` case contains a space:

```
last=tap:save; events=3; text=Sarah Connor; opaque=16.0,412.0,240.0,80.0
```

`opaque` is where the unannotated region landed after layout. The app reports
its own geometry so the driver can aim `ui_tap_at` without hard-coding a
layout that breaks the first time a font or a screen size changes.

## Checks

| # | Check | What it validates |
| --- | --- | --- |
| C1 | tree | The app is described: status, Save, Locked and Destination are present, and Save offers `tap`. Checked first and separately — an empty tree means semantics never came up, which is a different fault from a dispatch not working |
| C2 | dispatch | `ui_tap` invokes **the widget's own handler**. This is the claim the mock-host tests cannot make: they end where the shell begins |
| C3 | set_text | `ui_set_text` reaches the field's editing connection and replaces its contents. Nothing else in the suite covers it |
| C4a | exclusion | The `ExcludeSemantics` region is absent from the tree, so `ui_tap` has nothing to address |
| C4b | pointer | `ui_tap_at` at that region's rect reaches it anyway. C4a and C4b together show the contrast a dispatch cannot cross but a pointer can, demonstrated rather than argued |
| C5 | enabled gate | A disabled control is refused **and never reaches the widget** — the framework drops such an action silently, so a client would otherwise see success and no effect |
| C6 | flow | A multi-step flow driven only by what an agent can see: a control found by role and offered action, moved twice, then a custom action invoked by its label. Each step is verified from the action's own reply rather than by re-reading the tree |
| C7 | app tool | A tool the application declared, with its schema. The semantics verbs cannot express this — an agent names the value it wants instead of working out how many increments reach it |

C8–C11 run only when more than one application is live, because addressing is
the thing they cover and it does not exist with one:

| # | Check | What it validates |
| --- | --- | --- |
| C8 | addressing | Every application has its own tree resource, under a distinct name. Two sharing a name would leave neither addressable |
| C9 | routing | An action naming one application reaches **only** it. Both run the same bundle, so both have a node of the same id — the collision that makes an unaddressed dispatch land in the wrong place |
| C10 | refusal | An action naming no application is refused, and the refusal says a view was needed. An agent that omitted the argument has to be able to learn it exists |
| C11 | spanning read | A read naming none searches every application and says which one each result came from, since an agent hunting for a control does not yet know who owns it |

The names are the shell's own, not this fixture's: it derives them from the
bundle directory, disambiguating a collision with an index. That is why these
checks discover them instead of asserting them — the unit tests cover the
addressing logic with names they chose, and the derivation is the part only a
real shell can show.

## The app must be composited, or nothing works

This bit is not obvious and cost real time to find. If the surface is not
being composited -- occluded, on another workspace, or behind a lock screen --
then no frames run, `setState` never rebuilds, and **every check fails in a way
that looks like the dispatch path is broken**. The action does reach the
framework; the result simply never lands.

**Check the screen is unlocked first.** A locked desktop session stops
presenting frames deliberately, so a run started before locking will stall
partway through with the status frozen at whatever it last managed:

```bash
loginctl show-session "$(loginctl show-user "$USER" -p Display --value)" -p LockedHint
# LockedHint=no  -> frames are being presented
```

Run it under a compositor you control rather than a desktop session, which
sidesteps the lock policy as well as workspace and occlusion effects:

```bash
weston --backend=headless --width=900 --height=700 --socket=wl-mcp-test &
WAYLAND_DISPLAY=wl-mcp-test <shell> -b <bundle> --enable-mcp
```

If a check times out with the status frozen at an earlier value, suspect
presentation before suspecting the tool it was exercising.

## Running it

The shell must be built with `BUILD_MCP` (which requires `BUILD_ACCESSIBILITY`)
and started with `[global] enable_mcp = true`:

```bash
cmake -B build/mcp -GNinja \
  -D BUILD_ACCESSIBILITY=ON -D BUILD_MCP=ON
ninja -C build/mcp

emb -v cross . --backend software --build      # or your usual bundle step
./build/mcp/shell/homescreen -b <bundle> --enable-mcp
```

Then, in another terminal:

```bash
test/integration/mcp_drive_test/tools/drive.py
```

It prints one line per check and exits non-zero if any failed. The socket
defaults to `$XDG_RUNTIME_DIR/ivi-homescreen/mcp.sock`; pass `--socket` to
point at another, including one forwarded from a board over ssh.

### With more than one application

`--bundle` is repeatable, and each one gets its own engine and its own tree, so
the same bundle twice is enough to exercise addressing — and it is the harder
case, because both then derive the same name and the shell has to disambiguate:

```bash
./build/mcp/shell/homescreen -b <bundle> -b <bundle> --enable-mcp
```

The driver discovers how many are running and adapts: with one it leaves the
view argument off, which is the case the shell allows to go unqualified; with
several it names one for C1–C7 and adds C8–C11. No extra flag.

Two surfaces make the presentation problem above sharper rather than milder.
Under a headless compositor only one is generally composited, so checks against
the other time out with its status frozen — the failure looks identical to a
broken dispatch. Confirm both are presenting before reading a two-view failure
as a defect: tap one control twice and watch `events` climb.

## Why the driver keys on label and role, not identifier

The app sets `identifier` on every control, and the driver prefers it when the
engine reports one — but it matches on label and role first.

Flutter **3.38.3**, the oldest engine this port must run against, never
writes `identifier` at all. A fixture keyed on it would pass against a
development engine and fail against every shipping target, which is the
opposite of what an integration test is for.

## What this still does not cover

The checks drive the semantics and pointer paths. They do not exercise
`ui_scroll_to`, the AccessKit bridge, or notification delivery under load —
those have unit coverage and, for AccessKit, verification over a real AT-SPI
bus, but not against a real engine here.

Two gaps are specific to running several applications, both found by C8–C11 and
neither fixed here:

- **`ui_tap_at` cannot be aimed.** The host's pointer-tap seam takes a view and
  the shell discards it, so a synthetic tap goes wherever the ordinary input
  path sends it regardless of which application the caller named. C4b is
  therefore not meaningful with more than one running.
- **Tools an application declares are first-come.** The prefix is claimed
  process-wide, so a second application registering the same one is refused and
  its tools never appear. Semantics trees are addressed per application; these
  are not.

C4b depends on the region being unobscured. If the app grows a layout where
something overlaps it, that check will correctly start failing — a pointer
tap is hit-tested, and that is the property under test.
