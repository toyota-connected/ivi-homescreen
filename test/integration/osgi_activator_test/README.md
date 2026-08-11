# osgi_activator_test

Minimal OSGi bundle activator. This is the smallest thing that makes a bundle a
*bundle* rather than an ordinary Flutter app: it completes the `dev.osgi/bridge`
handshake and declares itself ACTIVE.

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

## Symbolic name

Read from `--dart-entrypoint-args`, so one build stands in for any bundle in a
config:

```toml
[[osgi.bundles]]
symbolic_name = "com.ivi.cluster"

  [osgi.bundles.args]
  dart = ["com.ivi.cluster"]
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

> The bundle ships `lib/libflutter_engine.so`, and **two bundles cannot each
> carry their own copy**: `LibFlutterEngine::Load()` is one-shot and keyed on the
> path it first binds, so the second bundle's engine load is refused however
> identical the file is. The harness hoists one shared copy out; a real
> deployment supplies the engine once, process-wide.
