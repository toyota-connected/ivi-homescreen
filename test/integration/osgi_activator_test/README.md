# osgi_activator_test

Minimal OSGi bundle activator. This is the smallest thing that makes a bundle a
*bundle* rather than an ordinary Flutter app: it completes the shell's handshake
— over `dev.osgi/bridge` or over the `ihs_osgi_*` C ABI — and declares itself
ACTIVE.

## Why it exists

A critical bundle's startup wait is released by ACTIVE and nothing else — not by
the engine coming up, which the shell already knows about, and not by the first
frame, which says nothing about whether the bundle's own code is ready.

An ordinary Flutter app never sends it, so before this app existed the shell
could only ever *time out* a critical bundle. That is why
`test/osgi_multi_bundle.sh` could not assert the critical-first ordering
guarantee — its `B3` case skipped, and would have skipped however correct the
shell was.

## The handshake

| | |
|---|---|
| `init` | Hands over the two things native code cannot obtain by itself: the address of `NativeApi.initializeApiDLData` (so the shell can bind `Dart_PostCObject_DL`) and this isolate's receive port. The shell posts the framework isolate's port back to that receive port whenever it knows it — which may be before or after this call returns. |
| `active` | Sent after the activator's own start-up work finishes. Here that work is trivial; in a real bundle it is whatever must be true before the bundle is fit to be seen. |

Both come from `dart:ffi`, not `dart:ui` — `NativeApi` and the `nativePort`
extension on `SendPort` both live there.

The framework port comes back as a **`SendPort`**, not as the port id that was
sent out. That asymmetry is forced: Dart offers no way to turn a port id back
into a `SendPort`, so an integer would leave the bundle with nothing it could
send to. The shell posts a `Dart_CObject_kSendPort` for exactly that reason, and
a bundle that tests the message for `int` will simply never see it.

## Two transports, one app

The second entrypoint argument picks the path: `channel` (the default) or `ffi`.

| | |
|---|---|
| `channel` | `dev.osgi/bridge`. Proven on hardware and needs nothing of the shell that is not already there — at the cost of a UI binding even for a headless bundle, and an ACTIVE report marshalled through the platform task runner during the busiest part of start-up, while the startup deadline is running. |
| `ffi` | `ihs_osgi_register_bundle` / `ihs_osgi_report_active` in `libihs_shared`, opened by its versioned SONAME. Synchronous, no binding, no task runner. Reports through an opaque handle the shell mints rather than a name, because any code in the process can reach those symbols. Needs `ENABLE_OSGI=ON`. |

One app rather than two, because a bundle directory must not ship its own
`lib/libflutter_engine.so` (see the note at the end) — a second activator bundle
would collide with this one for reasons unrelated to what is being tested.

The FFI path still runs a widget tree and still repaints. Its point is "no
channel, no platform thread", not headlessness: `B5` counts page flips, so a
bundle that stopped presenting would fail it.

Both paths end at the same `BridgeRegistry` and drive the same lifecycle state
machine, which is why every assertion in the harness is identical either way —
`B3` greps the state-machine transition (`bundle '…': STARTING -> ACTIVE`), not
a transport's own log line.

Bindings are hand-rolled rather than taken from the `osgi_ffi` package, which
lives in another repository: this fixture stays self-contained, exactly as the
channel path does not depend on `osgi_flutter`.

## Symbolic name

Read from `--dart-entrypoint-args`, so one build stands in for any bundle in a
config:

```toml
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"

  [osgi.bundles.args]
  dart = ["com.ivi.cluster", "ffi"]   # second arg optional; default "channel"
```

It **must** match the `[[osgi.bundles]]` entry: the shell refuses an `active`
report from a name it does not know, on the grounds that the likeliest cause is
a config mismatch and that should be loud rather than silent.

## It repaints on purpose

The 10 Hz ticker is not decoration. A Flutter tree with nothing changing
produces no damage and therefore no frames, so a static bundle stops
page-flipping within a few frames of start-up — and the harness's `B5` case,
which counts flips to prove both CRTCs are live, cannot tell a bundle that has
legitimately gone idle from one that never presented at all. A real cluster or
navigation view animates; this stands in for that.

## On screen

Amber while starting, green once ACTIVE, red if the handshake failed, with the
reason underneath — so a failure is visible on the panel rather than only in the
log.

## Building and running

```sh
emb bundle --app-path test/integration/osgi_activator_test \
    --arch arm64 -m release --build -o /tmp/activator-bundle
```

Then, on a target with two connected outputs and DRM master available (a
console session, not a desktop):

```sh
HOMESCREEN=./homescreen BUNDLE=/tmp/activator-bundle \
DRM_DEVICE=/dev/dri/cardN ACTIVATOR=1 COUNT_FLIPS=1 \
    test/osgi_multi_bundle.sh
```

`ACTIVATOR=1` tells the harness this bundle can report ACTIVE, which is what
lets it make the first bundle critical and assert `B3`.

To exercise the FFI path instead, add `TRANSPORT=ffi` — against a shell built
with `ENABLE_OSGI=ON`, so `libihs_shared` exports the `ihs_osgi_*` symbols:

```sh
HOMESCREEN=./homescreen BUNDLE=/tmp/activator-bundle \
DRM_DEVICE=/dev/dri/cardN ACTIVATOR=1 COUNT_FLIPS=1 TRANSPORT=ffi \
    test/osgi_multi_bundle.sh
```

The panel shows which path ran (`via channel` / `via ffi`), so a photograph of
the screen is enough to tell them apart without reading the log.

> The bundle ships `lib/libflutter_engine.so`, and **two bundles cannot each
> carry their own copy**: `LibFlutterEngine::Load()` is one-shot and keyed on the
> path it first binds, so the second bundle's engine load is refused however
> identical the file is. The harness hoists one shared copy out; a real
> deployment supplies the engine once, process-wide.
