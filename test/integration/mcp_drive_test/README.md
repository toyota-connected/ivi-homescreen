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
  -> ihs_mcp_host -> semantics provider     (routing)
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
| C3 | set_text | `ui_set_text` reaches the field's editing connection and replaces its contents. This is plan R-9's stated exit criterion, and nothing else in the suite covers it |
| C4a | exclusion | The `ExcludeSemantics` region is absent from the tree, so `ui_tap` has nothing to address |
| C4b | pointer | `ui_tap_at` at that region's rect reaches it anyway. C4a and C4b together are the DR-7 contrast, demonstrated rather than argued |
| C5 | enabled gate | A disabled control is refused **and never reaches the widget** — the framework drops such an action silently, so a client would otherwise see success and no effect |

## Running it

The shell must be built with `BUILD_MCP` (which requires `BUILD_ACCESSIBILITY`)
and started with `[global] enable_mcp = true`:

```bash
cmake -B build/mcp -GNinja \
  -D BUILD_ACCESSIBILITY=ON -D BUILD_MCP=ON
ninja -C build/mcp

emb -v cross . --backend software --build      # or your usual bundle step
./build/mcp/shell/ivi-homescreen -b <bundle> --enable-mcp
```

Then, in another terminal:

```bash
test/integration/mcp_drive_test/tools/drive.py
```

It prints one line per check and exits non-zero if any failed. The socket
defaults to `$XDG_RUNTIME_DIR/ivi-homescreen/mcp.sock`; pass `--socket` to
point at another, including one forwarded from a board over ssh.

## Why the driver keys on label and role, not identifier

The app sets `identifier` on every control, and the driver prefers it when the
engine reports one — but it matches on label and role first.

Flutter **3.38.3**, the deployment floor this port targets (plan §2.2), never
writes `identifier` at all. A fixture keyed on it would pass against a
development engine and fail against every shipping target, which is the
opposite of what an integration test is for.

## What this still does not cover

The checks drive the semantics and pointer paths. They do not exercise
`ui_scroll_to`, the AccessKit bridge, or notification delivery under load —
those have unit coverage and, for AccessKit, verification over a real AT-SPI
bus, but not against a real engine here.

C4b depends on the region being unobscured. If the app grows a layout where
something overlaps it, that check will correctly start failing — a pointer
tap is hit-tested, and that is the property under test.
