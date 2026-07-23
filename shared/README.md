# `ihs_shared`

The C-ABI shared library that fronts a small set of process-wide services for
**out-of-tree Dart FFI plugins**: logging, tracing, platform-view surface
negotiation, and configuration read-back. It ships as a single versioned shared
object, `libihs_shared.so`, and is the only supported binary interface a plugin
reaches across the `dlopen` boundary — everything on that boundary is plain C
(`<stdint.h>`/`<stddef.h>` types, errors by return code, a thread-local
`ihs_last_error_message()`), with any C++ convenience kept header-only and
inline over the C entry points. The boundary contract is
[docs/PLUGIN_ABI.md](../docs/PLUGIN_ABI.md); this document covers the library
itself. This surface is deliberately disjoint from the in-tree
`FlutterDesktopPluginRegistrar` / messenger surface used by statically
registered plugins — `ihs_shared` exposes neither a registrar nor a messenger.

## Features

| Capability | Status | Toggle / scope |
|---|---|---|
| Logging (generic `ihs_log_*` surface: ring registry, drain worker, console + file + level-floor sinks) | Built-in | Always compiled; `IhsApi::logging` always present |
| DLT logging sink | Optional | `ENABLE_DLT` (default `ON`); adds `dlt` as an `IHS_LOG_SINK` value, `dlopen`s `libdlt` at runtime |
| Tracing (`ihs_trace_*`: duration / instant / counter / flow) | Built-in | Always; engine trace procs → kernel `trace_marker` → nop |
| Platform-view surface negotiation (`ihs_pv_*`) | Built-in | Always; inert until the shell installs an `IhsPvHost` |
| Config read-back (`ihs_config_*` snapshots) | Built-in | Always; shell publishes, plugins read |
| Versioned ABI handshake (`ihs_get_api`) | Built-in | Always; `IHS_SHARED_ABI_VERSION` |

---

## Architecture

```mermaid
flowchart TD
    subgraph plugin["out-of-tree FFI plugin (.so)"]
      P["Dart FFI native lib"]
    end
    subgraph lib["libihs_shared.so.1 (one copy of process state)"]
      ENTRY["ihs_get_api() / ihs_last_error_message()"]
      LOG["logging: ring registry,\ndrain worker, sinks"]
      TR["trace: engine procs\n/ trace_marker"]
      PV["platform_view:\nnegotiate scoring (thin forwarder)"]
      CFG["config: refcounted snapshots"]
    end
    subgraph shell["ivi-homescreen shell (in-process)"]
      SINIT["ihs_log_start() at startup"]
      SPROCS["ihs_trace_set_engine_procs()"]
      SHOST["ihs_pv_set_host(IhsPvHost)"]
      SPUB["ihs_config_publish()"]
      REG["PlatformViewRegistry\n(owns id→view lifecycle)"]
    end

    P -->|"DynamicLibrary.open\n(\"libihs_shared.so.1\")"| ENTRY
    ENTRY --> LOG & TR & PV & CFG
    SINIT --> LOG
    SPROCS --> TR
    SHOST --> PV
    SPUB --> CFG
    PV -->|"IhsPvHost forward"| REG
```

The library is **shared-only, on purpose**. A static copy linked into both the
shell and a plugin would give the process two independent copies of the logging
bridge, the ring registry, and the trace sink table; a single `.so` with exactly
one copy of that state is the whole point of the boundary. The shell links
`ihs_shared` (via `ivi_homescreen::ihs_shared`) and brings process-wide state
online — starting the logging bridge, installing the engine trace procs, and
registering the platform-view host — before any plugin loads. A plugin reaches
the library either transitively (its own native library links it) or directly
via `DynamicLibrary.open("libihs_shared.so.1")`, the documented versioned
SONAME.

The four capabilities are surfaced two ways that alias the same code: flat C
entry points (`ihs_log`, `ihs_trace_instant`, `ihs_pv_negotiate`,
`ihs_config_get_int`, …) and function-pointer sub-tables reached through
`ihs_get_api()`. `ihs_get_api(requested_abi)` returns a process-lifetime
`IhsApi` table, or `NULL` when the requested **major** version is not the one
this library provides — so a plugin newer than the shell fails cleanly at load
rather than mismatching a struct layout at runtime. Each table leads with a
`size_t struct_size` (the Flutter embedder convention), and a sub-table pointer
is `NULL` when that capability is absent from the running build; a consumer
treats a null sub-table as "capability absent."

Module responsibilities:

- **`ihs_api.cc`** — the entry point. Validates the ABI major, then builds the
  `IhsApi` table on first call (so sub-table pointers resolve at runtime,
  avoiding a static-init-order dependency) and owns the thread-local
  `ihs_last_error_message()` string.
- **`logging/`** — the generic logging surface: a per-thread ring
  (`thread_ring`), a `ring_registry` drained by a background `worker`, a
  `context_cache`, and a pluggable `sink_set` (console, file, level-floor, and
  the optional DLT sink loaded through `libdlt_loader`). `ihs_log()` is
  wait-free (enqueue, drop-and-count on overflow); formatting and I/O happen on
  the drain thread; `ihs_log_flush()` is synchronous. `ffi_shim.cpp` exposes the
  logging sub-table.
- **`trace.cc`** — emits to the Flutter engine trace procs once the shell
  installs them, else to the kernel `trace_marker` (opened once at load so
  pre-engine bring-up is still captured), else nothing. A single relaxed-atomic
  enable flag gates every trace site so a disabled trace is near-free.
- **`platform_view.cc`** — a thin forwarder over the shell-installed `IhsPvHost`
  (`platform_view_host.h`). It carries no view state — the shell's
  `PlatformViewRegistry` owns the `id→view` lifecycle — so the only logic here
  is the pure best-to-floor negotiate scoring, which is unit-testable without a
  shell. With no host installed the surface is inert but well-defined.
- **`config.cc`** — reference-counted, generation-stamped snapshots. The shell
  publishes a flattened snapshot via a builder; a plugin acquires one (which
  stays valid across a config replacement until released) and reads typed values
  by flattened key, polling `ihs_config_generation` to notice a change.

### File map

| Path | What it is |
|------|-----------|
| [shared/CMakeLists.txt](CMakeLists.txt) | Builds `ihs_shared` (shared-only); install + CMake package export + pkg-config; `ENABLE_DLT` gate |
| [shared/ihs_shared.map](ihs_shared.map) | Linker version script — exports only `ihs_*`, everything else local |
| [shared/include/ihs/ihs.h](include/ihs/ihs.h) | `ihs_get_api()`, `ihs_last_error_message()`, the top-level `IhsApi` table |
| [shared/include/ihs/ihs_version.h](include/ihs/ihs_version.h) | `IHS_SHARED_ABI_VERSION` = `(major << 16) \| minor`; `IHS_ABI_MAJOR/MINOR` |
| [shared/include/ihs/logging.h](include/ihs/logging.h) | Logging C ABI + `IhsLoggingApi`; levels, context options, timing contract |
| [shared/include/ihs/trace.h](include/ihs/trace.h) | Tracing C ABI + `IhsTraceApi`; zero-cost macros and the C++ `TraceScope` RAII |
| [shared/include/ihs/platform_view.h](include/ihs/platform_view.h) | Platform-view plugin surface: kinds, requirements, grants, factory/callbacks, frame submit |
| [shared/include/ihs/platform_view_host.h](include/ihs/platform_view_host.h) | **Shell-only** host seam (`IhsPvHost`, `ihs_pv_set_host`) — not part of the plugin ABI |
| [shared/include/ihs/config.h](include/ihs/config.h) | Config read surface + builder (`IhsConfigApi`); refcounted snapshots by flattened key |
| [shared/include/ihs/format.h](include/ihs/format.h) | Header-only `ihs::format_to` (`std::format_to_n` or `snprintf`); C++ convenience, not ABI |
| `shared/include/ihs/ihs_export.h` | Generated at configure time by `generate_export_header` — defines `IHS_EXPORT` |
| [shared/src/ihs_api.cc](src/ihs_api.cc) | Entry point + capability-table assembly + last-error string |
| [shared/src/trace.cc](src/trace.cc) | Tracing implementation |
| [shared/src/config.cc](src/config.cc) | Config snapshot store + builder |
| [shared/src/platform_view.cc](src/platform_view.cc) | Platform-view forwarder + negotiate scoring |
| [shared/src/ihs_internal.hpp](src/ihs_internal.hpp) | Non-installed sub-table accessors shared between TUs |
| `shared/src/logging/` | Logging internals: ring, registry, worker, context cache, sinks, DLT loader, FFI shim |
| [shared/abi/libihs_shared.so.abi](abi/libihs_shared.so.abi) | Committed `libabigail` baseline — the CI ABI gate diffs against it |
| `shared/cmake/*.in` | CMake package config + pkg-config templates for downstream consumers |
| `shared/tests/consumer/` | Out-of-tree C11 smoke test against the *installed* package |
| `shared/tests/sink_integration/` | End-to-end logging-sink test across `IHS_LOG_SINK` / level / file configs |

### Threading model

- **Logging.** `ihs_log()` is non-blocking: it enqueues onto a per-thread ring
  and returns; a full ring drops the record and bumps a per-ring counter.
  Formatting and I/O run on a single background drain thread. `ihs_log_flush()`
  is synchronous. Because that worker owns a thread and per-thread ring TLS whose
  destructors must not fire against unmapped code, the library is pinned with
  `-z nodelete` and never unloaded even if the plugin that pulled it in is.
- **Tracing.** The enable check and the engine-procs pointer are relaxed/acquire
  atomics; emitting is safe from any thread. The `trace_marker` fd is opened once
  at load and only written thereafter.
- **Platform views.** The entire `ihs_pv_*` surface — factory install/remove,
  `negotiate`, the grant accessors, and every `IhsPvCallbacks` entry — is
  **platform-thread only**. An internal mutex guards only the installed host
  pointer.
- **Config.** Snapshots are reference-counted with atomics; acquire/read/release
  are safe from any thread and a snapshot outlives a concurrent republish.

---

## Build steps

`shared/` builds two ways. As part of the homescreen tree it is pulled in by the
top-level `add_subdirectory(shared)` and inherits the project, toolchain, and
`ENABLE_DLT`. It also builds **standalone** (for the CI ABI gate and the
out-of-tree consumer smoke): when `shared/` is the top-level project it
establishes its own `project()` and an `ENABLE_DLT` option.

### Dependencies

- A C++17 compiler (GCC or Clang) and CMake ≥ 3.16.
- `Threads` (pthreads) — always required, for the logging drain worker.
- **No** build-time DLT dependency: the DLT sink `dlopen`s `libdlt` at runtime,
  so `ENABLE_DLT=ON` links only `${CMAKE_DL_LIBS}`.

### Configure + build

Standalone:

```sh
cmake -B build/shared -S shared -G Ninja
ninja -C build/shared ihs_shared
```

As part of the full tree, `ihs_shared` is built by the normal top-level
configure; the target name is `ihs_shared` (alias `ivi_homescreen::ihs_shared`).

Install (produces the headers, the versioned `.so`, the CMake package, and a
pkg-config file):

```sh
cmake --install build/shared --prefix /path/to/prefix
```

Symbol hygiene is enforced at link time: hidden visibility presets plus the
`ihs_shared.map` version script mean only the `ihs_*` C ABI is exported; all C++
internals stay local. `-Wall -Wextra` are on.

### Build matrix

| Config | CMake flags | Effect |
|--------|-------------|--------|
| Default | `ENABLE_DLT=ON` | Generic logging + DLT sink compiled in; `dlt` available as an `IHS_LOG_SINK` (loads `libdlt` at runtime) |
| No DLT | `-DENABLE_DLT=OFF` | Generic logging still compiled in; `dlt` is simply not an available sink and logging falls back to console |

The generic logging surface, tracing, platform-view, and config are always
present regardless of flags; `ENABLE_DLT` gates only the DLT-specific loader and
sink.

---

## Running

`libihs_shared.so` is loaded into the ivi-homescreen process. The shell links it
directly and initializes process-wide state at startup — `ihs_log_start()`,
`ihs_trace_set_engine_procs()` once the engine proc table resolves,
`ihs_pv_set_host()` after the backend and `PlatformViewRegistry` exist — before
loading any plugin.

A plugin's native library consumes it one of two ways:

- **CMake package.** `find_package(ivi-homescreen-shared CONFIG REQUIRED)` then
  `target_link_libraries(<plugin> PRIVATE ivi_homescreen::ihs_shared)`. This is
  exactly what `shared/tests/consumer/` does from an empty tree.
- **pkg-config / Dart FFI.** The installed `ivi-homescreen-shared.pc` covers
  meson / Yocto / quick checks. Dart FFI code opens the library at runtime with
  `DynamicLibrary.open("libihs_shared.so.1")` — the versioned SONAME — and calls
  `ihs_get_api(IHS_SHARED_ABI_VERSION)`, checking for `NULL` (major mismatch)
  before using any sub-table.

The installed headers compile as strict C11 and parse under `ffigen`, so they
can back a generated Dart FFI binding directly.

### Env vars

The logging sinks are selected from the environment at `ihs_log_start()`:

| Env | Default | Effect |
|------|---------|--------|
| `IHS_LOG_SINK` | `dlt` when built with `ENABLE_DLT` (falls back to `console` if `libdlt` is unavailable), else `console` | Sink selection: `dlt` \| `console` \| `file` |
| `IHS_LOG_LEVEL` | `verbose` (accept everything) | Severity floor; more-verbose records are dropped |
| `IHS_LOG_FILE` | — | Path for the `file` sink |
| `IHS_LOG_FILE_MAX_BYTES` | — | Rotation size for the `file` sink |
| `IHS_LOG_FILE_MAX_FILES` | — | Rotation count for the `file` sink |

---

## Diagnostics/Debug

- **Last error.** Every `ihs_*` failure sets a thread-local, human-readable
  string retrievable with `ihs_last_error_message()` (never `NULL`; empty when
  there has been no failure). This is the first thing to check when
  `ihs_get_api()` returns `NULL` (ABI major mismatch) or a `negotiate` call
  fails.
- **Version handshake.** Confirm `ihs_get_api(IHS_SHARED_ABI_VERSION)` is
  non-`NULL` and `api->struct_size == sizeof(IhsApi)`; a newer major than the
  library provides is rejected by design.
- **Absent capabilities.** A `NULL` sub-table means the capability is not in the
  running build (e.g. logging in a hypothetical build without it) — check for
  `NULL` rather than assuming presence.
- **Logging drops.** If records go missing under load, the per-thread ring
  overflowed (records drop silently and bump a per-ring counter); reduce volume
  or gate with `ihs_log_enabled()` before formatting. Use `ihs_log_flush()` to
  force pending records out before inspecting a sink.
- **Tracing sink.** `ihs_trace_enabled()` reports whether *any* sink is active.
  With no engine procs installed, traces go to the kernel `trace_marker`
  (`/sys/kernel/tracing/trace_marker`); if neither is available, emits are nops.
- **ABI regressions.** The committed `abi/libihs_shared.so.abi` baseline is
  diffed by the CI ABI gate; a break there is an intentional major bump, not a
  slip.
- **Exported symbols.** `nm -D --defined-only libihs_shared.so.1` should list
  only `ihs_*`; anything else escaped the version script.
- **Out-of-tree checks.** `shared/tests/consumer/` (C11 smoke against the
  installed package) and `shared/tests/sink_integration/` (per-sink logging
  scenarios) build from an empty tree and prove the boundary as a real plugin
  would consume it.

---

## References

- [docs/PLUGIN_ABI.md](../docs/PLUGIN_ABI.md) — the boundary contract: the C
  rule, one-copy-of-state rationale, relationship to the in-tree plugin ABI, and
  versioning.
- [docs/specs/ARCHITECTURE.md](../docs/specs/ARCHITECTURE.md) §7.1–7.2 —
  out-of-tree plugins and where `ihs_shared` sits.
- [shell/configuration/README.md](../shell/configuration/README.md) — the shell
  configuration that is flattened and published into the config read surface.
