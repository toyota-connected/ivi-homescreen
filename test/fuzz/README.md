# Fuzz targets

Coverage-guided fuzzing of the surfaces that parse bytes from outside the
process. Built only when `BUILD_FUZZERS=ON`, which requires clang.

## Building

```bash
cmake -B build/fuzz -GNinja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -D BUILD_ACCESSIBILITY=ON -D BUILD_MCP=ON -D BUILD_FUZZERS=ON
ninja -C build/fuzz wayland_protocols   # generated headers, needed first
ninja -C build/fuzz fuzz_mcp_transport
```

`BUILD_FUZZERS` instruments the **whole build**, not just the target — the
library under test has to carry the coverage counters or the fuzzer runs
blind. A run whose first `INITED` line reports coverage in the tens is
measuring its own driver and nothing else; a working one starts in the
thousands. Nothing about a blind run looks broken, which is why it is worth
checking the number rather than the exit status.

Use a build directory of its own. `BUILD_FUZZERS` and the `SANITIZE_*` options
refuse to configure together, since two `-fsanitize` lists on one link either
fail or silently drop one of them.

## Running

Keep the checked-in corpus read-only and let the fuzzer grow a working one:

```bash
mkdir -p /tmp/ihs-fuzz-corpus
./build/fuzz/fuzz/fuzz_mcp_transport \
    /tmp/ihs-fuzz-corpus test/fuzz/corpus/mcp_transport \
    -max_total_time=900 -print_final_stats=1
```

The first directory is the one libFuzzer writes to. Passing the seed corpus
first instead means new units land in the source tree, mixed in with the
curated seeds and no longer distinguishable from them.

To replay one input:

```bash
./build/fuzz/fuzz/fuzz_mcp_transport test/fuzz/corpus/mcp_transport/tools_call
```

## `fuzz_mcp_transport`

The input is the entire client side of one connection — request line, headers
and body — written to the transport's Unix socket unmodified.

Driving it through the socket rather than calling the parsers directly is the
point. The header block, the `Content-Length` accounting, the JSON-RPC
envelope and the routing onto a provider see the same bytes in the same order
a client would send them, so a bug that lives in how they interact is
reachable. It also keeps the target honest about what is actually exposed:
these functions live in an anonymous namespace, and reaching them any other
way would mean widening their visibility for the benefit of a test.

Why this surface earns the machinery: peer credentials are the whole local
trust model, and they are strong. But off-box access terminates TLS in front
of the socket (`docs/mcp-remote-access.md`), and past that terminator the
peer uid is the terminator's own. Every byte here came from somewhere else,
and the parser runs before any of it is understood.

The target registers a provider of its own so `tools/list`, `tools/call`,
`resources/read` and the subscription paths route somewhere. Without it most
of the dispatcher is unreachable — every call bottoms out in "no such tool"
before the interesting code runs.

One thing the driver does that is about the driver, not the transport: it
drains the reply before closing. Closing on a partly-read response resets the
connection before the server finishes writing, which would turn a real reply
path into an untested one.

It deliberately does **not** work around anything else. An earlier version
restarted the transport after every event-stream handshake, because an
abandoned stream's descriptor was held until a write to it failed and millions
of executions turned that into descriptor exhaustion. That was a real defect,
it is fixed, and the workaround is gone — a driver that compensates for the
bug it exists to find will go on hiding the regression.

### What it found

The first sustained run surfaced three defects of one shape: client-controlled
state with no bound and nothing to scope it to, since every request is
`Connection: close` and a connection is therefore not a session. Abandoned
event streams held their descriptors forever; `Mcp-Session-Id` was never
checked against the ids the transport had minted, so the subscription map grew
without limit and one client could name another's session; and nothing bounded
the stream count, the subscriptions per session, or a URI's length. All are
pinned by tests in `test/unit_test/mcp_transport-test/`.

## Corpus

`corpus/mcp_transport/` holds hand-written seeds: one per JSON-RPC method,
plus the HTTP-level cases worth starting from (event-stream handshake, plain
`GET`, an `Origin` header, a missing/unparseable/oversized `Content-Length`, a
body shorter than declared, mixed-case header names).

Seeds are for reaching code, not for asserting behaviour. Anything the fuzzer
finds that should stay found belongs in a unit test beside the code it pins —
`test/unit_test/mcp_transport-test/` — where it runs in CI and says what the
correct answer is. A crashing input parked in a corpus directory only says
that it once crashed.

These targets are deliberately not registered with CTest: a fuzz target runs
until it is stopped, so a test suite would either never finish or would run it
for a token second and report a pass that means nothing.
