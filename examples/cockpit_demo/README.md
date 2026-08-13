# Cockpit demo

A Flutter app for driving ivi-homescreen from an LLM. It exists to make the
two routes into a UI visible side by side:

| | Where it comes from | What an agent gets |
| --- | --- | --- |
| **Generic verbs** (`ui_tap`, `ui_query`, `ui_increase`, …) | The semantics tree, automatically | Anything the app is accessible enough to describe. Costs the app nothing |
| **Declared tools** (`cockpit_set_temperature`, …) | The app, via `ihs_mcp_app_tools` | Typed operations with schemas: *set the passenger zone to 23.5* rather than pressing "warmer" an unknown number of times |

Both act on the same state. Two `ui_tap` calls on the "Fan faster" button and
one `cockpit_set_fan` call are indistinguishable afterwards, which is the
point: an agent picks whichever fits, and a well-annotated app is drivable
even before anyone declares a tool.

Everything an agent does appears in the on-screen activity log, so a person
watching can see what happened and in which order.

## Running it

Build the shell with the MCP surface, and the app:

```bash
cmake -B build/mcp -GNinja -D BUILD_ACCESSIBILITY=ON -D BUILD_MCP=ON
ninja -C build/mcp

cd examples/cockpit_demo && flutter build bundle && cd -
```

Assemble a bundle directory (`data/flutter_assets`, `data/icudtl.dat`,
`lib/libflutter_engine.so`) and start the shell with the surface enabled:

```bash
./build/mcp/shell/homescreen -b <bundle> --enable-mcp
```

**The app must be composited.** If the surface is occluded, on another
workspace, or behind a lock screen, no frames run, nothing rebuilds, and every
action appears to fail while actually having reached the framework. A locked
screen stops presenting frames deliberately — check before concluding
something is broken:

```bash
loginctl show-session "$(loginctl show-user "$USER" -p Display --value)" -p LockedHint
```

## Connecting Claude

The shell serves MCP on a Unix socket, and no MCP client dials one directly,
so `scripts/mcp_stdio_bridge.py` adapts it to stdio.

**Claude Code:**

```bash
claude mcp add cockpit -- python3 /path/to/ivi-homescreen/scripts/mcp_stdio_bridge.py
```

**Claude Desktop** — in its MCP configuration:

```json
{"mcpServers": {"cockpit": {"command": "python3",
  "args": ["/path/to/ivi-homescreen/scripts/mcp_stdio_bridge.py"]}}}
```

Then ask for something the UI can do:

- *"What is the cockpit showing right now?"*
- *"Set the passenger side to 23 and turn the fan down."*
- *"Play some music and set the destination to the airport."*
- *"Turn the fan up using only the on-screen buttons, not the typed tools."*

The last one is worth trying: it forces the generic path, and you can watch
the agent find the button by label and tap it.

### claude.ai in a browser

**Not directly.** The browser client connects to *remote* MCP servers over
HTTPS; it cannot spawn a local process or reach a Unix socket. The stdio
bridge above serves Claude Code and Claude Desktop, which run on the same
machine as the shell.

Reaching it from the browser means exposing the socket to the network, which
is a deliberate decision rather than a missing feature — see
`docs/mcp-remote-access.md`, which covers terminating mTLS in front of the
socket and the two things that arrangement makes easy to get wrong.

## What to look at in the source

- `_registerTools()` — the declared tools, each with a JSON Schema. A handler
  that throws reports the message to the agent, which is how a tool says it
  ran and could not do the thing.
- `MergeSemantics` around every control. A bare `Semantics` wrapper puts the
  label on a *parent* node and leaves the actionable node unnamed, so an agent
  that resolves the label finds a node offering nothing. This is the single
  most common annotation mistake and it is silent.
- `ExcludeSemantics` on the activity log. Without it, the agent's view of the
  UI fills with a transcript of its own actions.
- `dispose()` unregisters the tools. A tool left advertised after its handler
  is gone fails only after the call times out, which reads as the whole
  surface being broken.
