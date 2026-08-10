# Vendored Dart embedding headers

Source: https://github.com/dart-lang/sdk — `runtime/include/`

| | |
|---|---|
| Dart SDK revision | `d684a576a6aa954ae107a03b2b4e1d61c3bebe93` |
| Pinned by | `flutter/flutter` tag `3.44.2`, `DEPS` → `dart_revision` |
| Matches | `.flutter-version` (3.44.2) |
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

## Why the whole set was refreshed together

`dart_native_api.h` at this revision uses `DART_API_WARN_UNUSED_RESULT`, which
the previously vendored `dart_api.h` did not define (it had the older
`DART_WARN_UNUSED_RESULT` spelling). Mixing revisions does not compile, so the
whole include set moves together.

Nothing in the tree consumed the previous `dart_api.h` / `dart_tools_api.h`:
`fml/trace_event.h` is the only includer and nothing includes *it*. The refresh
is therefore behavior-neutral for existing code.

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
