# MCP surface: security review

What the MCP surface exposes, what stops it, and what an integrator has to
decide. Read alongside [`mcp-remote-access.md`](mcp-remote-access.md), which
covers reaching the socket from off the box.

The short version: **this lets an external process drive the user interface.**
That is the feature. Everything below is about who gets to.

## What is exposed

| | |
| --- | --- |
| **Read** | The entire semantics tree of every running application — every label, value, role and screen rectangle each publishes for accessibility. One resource per application, `ui://semantics/{view}/tree` |
| **Act** | Any semantics action the application offers on any node: taps, text entry, scrolling, and the application's own custom actions |
| **Coordinates** | `ui_tap_at` synthesizes a hit-tested tap at a point, reaching what the tree does not describe |
| **Application tools** | Whatever an application declares through `ihs_mcp_app_tools.h`, under its own prefix |

Treat the tree as **everything on screen**. An agent that can read it can read
the user's destination, their media, and any text a field displays — and where
the shell runs several applications, it can read all of them. There is no
per-application access control: admitting a client admits it to every view.

## What stops it

### It is off unless twice enabled

`BUILD_MCP=ON` at build time, and `enable_mcp` in configuration at runtime.
Both default off, and the build gate controls sources, exported symbols and
installed headers — a default build exports none of it, so an image that never
opted in cannot be talked into it by configuration alone.

### The socket is the boundary

A Unix domain socket, 0600, in a 0700 directory. No port is bound and none
will be.

The mode is narrowed by `umask` across `bind()` rather than by a `chmod`
afterwards. Between a permissive bind and a later `chmod` the socket is
briefly connectable by anyone, and a window that small is still a window.

Two layers guard it and they fail in different circumstances:

- **The file mode** keeps other users from opening it at all. This is the
  layer that works when deployment is correct.
- **`SO_PEERCRED`** checks the connecting peer's uid against the shell's own,
  before a byte is read. This is what remains when the mode is wrong — a
  directory loosened by a deployment script, an image that relaxes permissions
  to make something else work.

A kernel-attested peer uid is a stronger statement than any token the
transport could carry instead, which is why there is no token.

Both layers are tested separately (`AnotherUserCannotOpenTheSocket`,
`AnotherUserIsRefusedOnceTheModeIsWrong`). Proving one proves nothing about
the other.

### Accessibility focus is not reachable

The actions that move a screen reader's cursor are absent from the tool table
**and** masked at the hub's dispatch funnel. An agent driving the interface
must not be able to yank the cursor away from someone reading the screen. Two
layers because the tool table is a list and lists get added to.

### Obscured fields do not round-trip

A node whose role is `password_input`, or whose obscured flag is set, never
has its `value` serialized. `ui_set_text` refuses an obscured target outright:
an agent that can type into a password field can read back what it typed
through the application's own behaviour, so suppressing the read alone would
not have closed it.

### Nothing grows without a bound

Every request is `Connection: close`, so a connection is not a session and
there is nothing for per-client state to be scoped to. Each limit below exists
because the absence of it was reachable by a client in a loop.

| Limit | Value | What it prevents |
| --- | --- | --- |
| Request body | 1 MiB | Refused before it is read |
| Header section | 16 KiB | Headers are read before any length is known |
| Open event streams | 32 | Each holds a descriptor; descriptors are process-wide |
| Live sessions | 64, evicted least-recently-used | `initialize` is otherwise a growth path |
| Subscriptions per session | 64 | |
| Subscribed URI length | 512 bytes | Held for as long as the session is |
| Application tool call | 5 s | An application that never answers |

A hung-up event stream is reaped on the accept thread's next wake rather than
when a write to it next fails, because that write needs a notification that
may never come.

### Sessions are checked, not accepted

`Mcp-Session-Id` is validated against the ids the transport minted. The header
is client-written, so without the check "session" is a namespace the client
owns — unbounded, and shared, letting one client name another's id and drop
its subscriptions.

### Origin is refused

Any request carrying an `Origin` header is refused. Only a browser sends one
and none is a legitimate client, so its presence means a rebound page is
talking to the socket through a terminator that forwards plain HTTP. The event
stream matters more than the request path here: without the check, a `GET`
opens a live feed of the interface rather than making a single call.

## What is fuzzed

Two targets, both under `test/fuzz/` (`BUILD_FUZZERS=ON`, clang only):

- **`fuzz_mcp_transport`** — the whole client side of one connection, written
  to the socket unmodified: header parsing, `Content-Length` accounting, the
  JSON-RPC envelope, and routing onto a provider.
- **`fuzz_mcp_semantics_tools`** — the `ui_*` tool arguments against a
  published tree: argument decoding, node resolution, state gating, dispatch.

Between them they found four defects, every one of a single shape:
**client-controlled input trusted further than it was checked.** The stream
and session leaks, and the argument parser reading past the length it was
given while scanning for a key it could not distinguish from a substring.

Anything a fuzzer finds is pinned by a unit test beside the code, not by a
corpus entry. A crashing input parked in a corpus says only that it once
crashed.

## What an integrator has to decide

### Off-box access

Nothing here terminates TLS. Certificate provisioning, CA trust, rotation and
cipher policy have their own lifecycle and belong to the product rather than
to a UI shell, so a terminator runs in front of the socket. See
[`mcp-remote-access.md`](mcp-remote-access.md), and note the two things that
arrangement makes easy to get wrong: **the terminator must run as the shell's
user** or `SO_PEERCRED` refuses everything, and **past it the client
certificate is the only remaining gate**, because peer credentials can no
longer tell an agent from the terminator.

### Which tools the surface offers

`mcp_allowed_tools` lists the UI tools an image permits, unprefixed. Absent
offers all of them, which is the compiled default and is already correct — it
excludes the accessibility-focus actions, which are never reachable whatever
is listed.

```toml
[global]
enable_mcp = true
mcp_allowed_tools = []                      # read-only: an agent can look, not touch
mcp_allowed_tools = ["tap", "scroll_to"]    # or exactly what the product needs
```

`snapshot` and `query` are always offered, because reading is what the tree is
for and a surface that cannot be read has no purpose.

Three things worth knowing about how this behaves:

- **It narrows and cannot widen.** The compiled default is the ceiling.
- **A name matching no tool refuses startup**, leaving the surface absent
  rather than open. A typo must not quietly grant less than intended, and
  least of all more.
- **The advertised list is the gate**, not a suggestion: the registry resolves
  every call against it before routing. That is what makes narrowing effective
  for `ui_tap_at`, which sends a pointer event rather than a semantics action
  and so has nothing for the hub mask to enforce.

Configuration is world-readable and unauthenticated. On a production image its
integrity is the image's, which is the same line drawn everywhere else here —
the shell owns what only it can, and the product owns trust material.

## Residual risk, stated plainly

- **An agent that reaches the socket can do anything `mcp_allowed_tools`
  permits**, which unset is everything the user can do short of moving the
  accessibility cursor or typing into an obscured field. The policy is
  per-image, not per-client: there is no way to admit one agent and restrict
  another, and no audit trail beyond the trace log.
- **Past a terminator, peer credentials mean nothing.** Every client the
  terminator admits is, to this transport, the same client.
- **The tree is as revealing as the application makes it.** An application
  that puts sensitive text in a label publishes it. Nothing here can tell a
  label apart from a secret.
- **`ui_tap_at` is not gated by node state**, because it consults no node. It
  can reach a control the tree deliberately does not describe.

## Review checklist

For an image shipping this surface:

- [ ] `enable_mcp` is off unless the product requires it
- [ ] The socket's directory is 0700 and owned by the shell's user
- [ ] No deployment step relaxes permissions on the runtime directory
- [ ] If a terminator is used, it runs as the shell's user and requires a
      client certificate
- [ ] `mcp_allowed_tools` is set for the image, not left implicit
- [ ] The application publishes no secret as a label or value
- [ ] Applications declaring their own tools have had those tools reviewed —
      they are outside everything above
