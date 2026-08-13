# ihs_mcp_app_tools

Declare typed MCP tools from a Flutter application running on ivi-homescreen.

The shell already exposes generic verbs over the semantics tree — tap this
node, set that field — and they need no cooperation from the application. This
package is for the operations that have no generic form:

```dart
final registration = McpAppTools.register(
  prefix: 'hvac_',
  tools: <McpTool>[
    McpTool(
      name: 'set_temp',
      description: 'Set a zone to a temperature in celsius.',
      inputSchema: <String, Object?>{
        'type': 'object',
        'properties': <String, Object?>{
          'zone': <String, Object?>{'type': 'string'},
          'celsius': <String, Object?>{'type': 'number'},
        },
        'required': <String>['zone', 'celsius'],
      },
      handler: (Map<String, Object?> args) async {
        await climate.set(args['zone']! as String,
                          (args['celsius']! as num).toDouble());
        return <String, Object?>{'ok': true};
      },
    ),
  ],
);
```

An agent then calls `hvac_set_temp` with named arguments, instead of working
out how many times to press a control it found by role.

## When to reach for this

Use the semantics verbs where they fit: they cost the application nothing, and
an accessible app is drivable with no extra work. Declare a tool when the
operation cannot be expressed as a sequence of taps on what is currently
drawn — because it takes parameters, because it spans several screens, or
because the UI for it is not on screen yet.

## What it binds to

`libihs_shared.so.1`, which is the supported binary interface between the
shell and out-of-tree Dart code (see `docs/PLUGIN_ABI.md`). No platform
channel is involved, and the library is opened by SONAME so it resolves to the
copy the shell has already loaded — opening a second one would give the
plugin its own registry and the tools would register with nothing.

Requires the shell to be built with the MCP surface and started with it
enabled. If it was not, `register` throws and an application that wants to
degrade rather than fail should catch it.

## Lifetime

Call `unregister()` when the surface offering the tools goes away, typically
in `State.dispose`. **A stale tool is worse than no tool**: an agent will call
it and be told nothing answered, seconds later, after the timeout.

Calls in flight when you unregister are failed immediately rather than left to
time out.

## How a call reaches you

The shell invokes on the thread serving the request, not the platform thread,
so the callback is a `NativeCallable.listener` and your handler runs on the
isolate's event loop afterwards. It may be `async`: the call is answered when
the future completes, and nothing blocks in the meantime.

Throwing marks the call failed and returns the message. That is distinct from
the tool not existing, which the client is told separately — a difference
worth preserving, because a client that cannot tell them apart retries the
wrong one.
