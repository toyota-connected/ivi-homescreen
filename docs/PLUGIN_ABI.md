# ivi-homescreen plugin ABI (`ihs_shared`)

`ihs_shared` is the supported binary interface between the ivi-homescreen
process and out-of-tree Dart FFI plugins. It is delivered as a single shared
library, `libihs_shared.so`, and covers logging, tracing, platform-view surface
negotiation, and configuration read-back.

## The boundary is C

Everything a plugin reaches across the `dlopen` boundary is plain C:

- No C++ types, exceptions, or RTTI cross the boundary. Any C++ convenience
  wrapper is header-only and inline over the C entry points, so the compiled
  boundary stays a C ABI.
- Types in the installed headers are fixed-width (`<stdint.h>`) and
  size (`<stddef.h>`); the headers compile as C11 and parse under `ffigen`.
- Errors are reported by return code. A thread-local
  `ihs_last_error_message()` carries a human-readable description for
  diagnostics. There are no errno-style globals.

## One library, one copy of state

`libihs_shared.so` is shared-only. There is no static variant, by design: a
static copy linked into the shell and another into a plugin would give the
process two independent copies of the logging bridge, ring registry, and trace
sink table. A single shared object with exactly one copy of process state is
the whole point of the boundary.

The shell links `ihs_shared` and initializes process-wide state (for example,
starting the logging bridge) before any plugin is loaded. Plugins reach the
library either transitively (their own native library links it) or directly via
`DynamicLibrary.open("libihs_shared.so.1")` — the versioned SONAME is the
documented open string.

## Relationship to the in-tree plugin ABI

`ihs_shared` does **not** replace or duplicate
`shell/platform/homescreen/public/flutter_homescreen.h` and the
`FlutterDesktopPluginRegistrar` / messenger surface. That surface remains the
interface for in-tree and statically registered plugins. `ihs_shared` covers a
disjoint set of concerns — logging, tracing, platform-view surface
negotiation, and config read — and does not expose a registrar or a messenger.
Keeping the two surfaces non-overlapping is deliberate: a texture path or
dmabuf path duplicated across both would drift into two half-interfaces.

## Versioning

The library exposes a single ABI version, `IHS_SHARED_ABI_VERSION`, a `uint32`
laid out as `(major << 16) | minor`:

- **Additive** changes (new entry points, new trailing struct fields guarded by
  a leading `struct_size`) bump the **minor** version. A plugin built against an
  older minor keeps working against a newer library by construction.
- **Breaking** changes bump the **major** version and the library `SOVERSION`.

The entry point is `ihs_get_api(uint32_t requested_abi)`. It returns a table
valid for the process lifetime, or `NULL` if the requested **major** version is
not the one this library provides — so a plugin newer than the shell fails
cleanly at load rather than mismatching a struct layout at runtime. Every table
and sub-table begins with a `size_t struct_size` field, following the Flutter
embedder convention already used throughout the shell.

A sub-table pointer in `IhsApi` is `NULL` when that capability is not present in
the running build. A consumer must treat a null sub-table as "capability
absent" rather than assuming it is always available.
