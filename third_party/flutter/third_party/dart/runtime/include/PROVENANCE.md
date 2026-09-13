# Vendored Dart embedding headers

Source: https://github.com/dart-lang/sdk — `runtime/include/`

| | |
|---|---|
| Dart SDK revision | `60a57cd42d64dc03e9f07aa60a2e250755c1ef28` |
| Pinned by | `flutter/flutter` tag `3.47.2`, `DEPS` → `dart_revision` |
| Matches | `.flutter-version` (3.47.2) |
| `DART_API_DL` ABI | major 2, minor 6 (`dart_version.h`) |

## Files

| File | Purpose |
|---|---|
| `dart_api.h` | Core embedding API types (`Dart_Handle`, `Dart_Port`, …) |
| `dart_tools_api.h` | Timeline/tools API; used by `fml/trace_event.h` |
| `dart_native_api.h` | `Dart_CObject`, `Dart_PostCObject` — message posting |
| `dart_api_dl.h` | Dynamically-linked API declarations |
| `dart_api_dl.c` | DL symbol table initializer; **must be compiled in** |
| `dart_version.h` | `DART_API_DL_{MAJOR,MINOR}_VERSION` |
| `internal/dart_api_dl_impl.h` | DL function-pointer table |

## Why the whole set moves together

These headers reference each other's macros, so a mixed set does not
necessarily compile. The 3.44.2 pin proved it: `dart_native_api.h` at that
revision used `DART_API_WARN_UNUSED_RESULT`, which the `dart_api.h` vendored
alongside it did not define — it had the older `DART_WARN_UNUSED_RESULT`
spelling. Every re-pin therefore refreshes all seven files, including any that
happen to be byte-identical.

## This refresh (3.44.2 → 3.47.2)

Four files changed and three came back identical — `dart_api_dl.c`,
`dart_version.h` and `internal/dart_api_dl_impl.h`, which are the DL surface
itself. The `DART_API_DL` ABI is unchanged at major 2, minor 6, so
`Dart_InitializeApiDL()`'s runtime check against the loaded engine sees the same
version it did before.

Only two headers are consumed in this tree, and neither consumption is at risk:

- `dart_api_dl.h` — included by `shell/osgi/bridge_registry.cc`, the OSGi
  bridge. Its change is additive: `#include <stdint.h>` and a blank line.
- `dart_tools_api.h` — included only by `fml/trace_event.h`, which no
  CMake target compiles and nothing else includes.

`dart_api.h` accounts for most of the diff (195 lines, including 27 removed
declarations such as `DART_FLAGS_CURRENT_VERSION` and the `kVmSnapshot*` symbol
names) and is included by nothing in the tree, so those removals reach no
consumer here.

Verified by configuring with `ENABLE_OSGI=ON` — which is what enforces the
presence check below — and compiling `dart_api_dl.c` and `bridge_registry.cc`
against the new headers.

## Updating

When `.flutter-version` changes, re-pin from the corresponding
`flutter/flutter` tag:

```sh
REV=$(curl -sSf "https://raw.githubusercontent.com/flutter/flutter/$(cat .flutter-version)/DEPS" \
      | sed -n "s/.*'dart_revision': '\([0-9a-f]\{40\}\)'.*/\1/p")
BASE="https://raw.githubusercontent.com/dart-lang/sdk/$REV/runtime/include"
for f in dart_api.h dart_tools_api.h dart_native_api.h \
         dart_api_dl.h dart_api_dl.c dart_version.h; do
  curl -sSf "$BASE/$f" -o "$f"
done
curl -sSf "$BASE/internal/dart_api_dl_impl.h" -o internal/dart_api_dl_impl.h
```

`Dart_InitializeApiDL()` version-checks against the running VM at runtime, so a
skew between these headers and the loaded `libflutter_engine.so` fails loudly
at init rather than corrupting memory. Keep them pinned regardless.
