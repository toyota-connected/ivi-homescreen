// Declaring MCP tools from a Flutter application.
//
// The shell already exposes generic verbs over the semantics tree: tap this,
// set that. This is for the operations that have no generic form -- a typed
// hvac_set_temp(zone, celsius) with a schema an agent can read, rather than a
// sequence of taps on whatever is currently drawn.
//
// It binds libihs_shared directly. That library is the supported binary
// interface between the shell and out-of-tree Dart code, so no platform
// channel is involved and registration costs one FFI call.
//
//   final registration = McpAppTools.register(
//     prefix: 'hvac_',
//     tools: <McpTool>[
//       McpTool(
//         name: 'set_temp',
//         description: 'Set a zone to a temperature in celsius.',
//         inputSchema: <String, Object?>{
//           'type': 'object',
//           'properties': <String, Object?>{
//             'zone': <String, Object?>{'type': 'string'},
//             'celsius': <String, Object?>{'type': 'number'},
//           },
//           'required': <String>['zone', 'celsius'],
//         },
//         handler: (Map<String, Object?> args) async {
//           setTemperature(args['zone']! as String,
//                          (args['celsius']! as num).toDouble());
//           return <String, Object?>{'ok': true};
//         },
//       ),
//     ],
//   );
//
// Call registration.unregister() when the surface offering the tools goes
// away -- typically in State.dispose. Leaving stale tools advertised is worse
// than offering none, because an agent will call one and be told, several
// seconds later, that nothing answered.

import 'dart:async';
import 'dart:convert';
import 'dart:ffi' as ffi;
import 'dart:io' show Platform;

import 'package:ffi/ffi.dart';

/// One tool the application offers.
class McpTool {
  McpTool({
    required this.name,
    required this.description,
    required this.inputSchema,
    required this.handler,
  });

  /// Unprefixed, as the application thinks of it: `set_temp`. The
  /// registration's prefix is applied by the shell.
  final String name;

  /// What an agent reads when choosing between tools. Worth writing properly:
  /// it is the only thing distinguishing two tools that look alike.
  final String description;

  /// JSON Schema for the arguments, passed to clients as declared.
  final Map<String, Object?> inputSchema;

  /// Runs the tool. The returned value is encoded as the result; return null
  /// for an empty one.
  ///
  /// Throwing marks the call failed and sends the message back, which is how a
  /// tool says it ran and could not do the thing -- distinct from the tool not
  /// existing, which the client is told separately.
  final FutureOr<Object?> Function(Map<String, Object?> arguments) handler;
}

// ---------------------------------------------------------------------------
// The C ABI (ihs/ihs_mcp_app_tools.h)
// ---------------------------------------------------------------------------

final class _AppTool extends ffi.Struct {
  external ffi.Pointer<Utf8> name;
  external ffi.Pointer<Utf8> description;
  external ffi.Pointer<Utf8> inputSchemaJson;
  @ffi.Uint64()
  external int capability;
}

final class _AppToolsDesc extends ffi.Struct {
  @ffi.Size()
  external int structSize;
  external ffi.Pointer<Utf8> toolPrefix;
  external ffi.Pointer<_AppTool> tools;
  @ffi.Size()
  external int toolCount;
  external ffi.Pointer<ffi.NativeFunction<_InvokeNative>> invoke;
  external ffi.Pointer<ffi.Void> userData;
  @ffi.Uint32()
  external int callTimeoutMs;
  external ffi.Pointer<Utf8> view;
}

typedef _PrefixNative = ffi.Pointer<Utf8> Function(ffi.Pointer<ffi.Void>);
typedef _PrefixDart = ffi.Pointer<Utf8> Function(ffi.Pointer<ffi.Void>);

typedef _InvokeNative = ffi.Void Function(
  ffi.Pointer<ffi.Void>,
  ffi.Uint64,
  ffi.Pointer<Utf8>,
  ffi.Pointer<Utf8>,
  ffi.Size,
);

typedef _RegisterNative = ffi.Int32 Function(
    ffi.Pointer<_AppToolsDesc>, ffi.Pointer<ffi.Pointer<ffi.Void>>);
typedef _RegisterDart = int Function(
    ffi.Pointer<_AppToolsDesc>, ffi.Pointer<ffi.Pointer<ffi.Void>>);

typedef _CompleteNative = ffi.Int32 Function(
    ffi.Uint64, ffi.Bool, ffi.Pointer<Utf8>, ffi.Size);
typedef _CompleteDart = int Function(int, bool, ffi.Pointer<Utf8>, int);

typedef _UnregisterNative = ffi.Void Function(ffi.Pointer<ffi.Void>);
typedef _UnregisterDart = void Function(ffi.Pointer<ffi.Void>);

class _Bindings {
  _Bindings._(ffi.DynamicLibrary library)
      : register = library.lookupFunction<_RegisterNative, _RegisterDart>(
            'ihs_mcp_app_tools_register'),
        complete = library.lookupFunction<_CompleteNative, _CompleteDart>(
            'ihs_mcp_app_tools_complete'),
        prefixOf = library.lookupFunction<_PrefixNative, _PrefixDart>(
            'ihs_mcp_app_tools_prefix'),
        unregister = library.lookupFunction<_UnregisterNative, _UnregisterDart>(
            'ihs_mcp_app_tools_unregister');

  // The versioned SONAME is the documented open string: the unversioned
  // symlink is a development-package artifact and is often absent on a target.
  static const String _soname = 'libihs_shared.so.1';

  factory _Bindings.open() {
    if (!Platform.isLinux) {
      throw UnsupportedError(
          'ihs_mcp_app_tools requires ivi-homescreen, which is Linux-only');
    }
    // Opened by name rather than by path: the shell has already loaded this
    // library, so this resolves to the copy already in the process. Loading a
    // second one would give the plugin its own registry, and the tools would
    // register with nothing.
    final ffi.DynamicLibrary library = ffi.DynamicLibrary.open(_soname);
    return _Bindings._(library);
  }

  final _RegisterDart register;
  final _CompleteDart complete;
  final _PrefixDart prefixOf;
  final _UnregisterDart unregister;
}

_Bindings? _bindings;
_Bindings get _abi => _bindings ??= _Bindings.open();

/// Status codes from the C ABI, for the failures worth naming.
const int _ok = 0;
const int _errPrefixTaken = -2;

/// A live registration. Tools stay advertised until [unregister].
class McpAppTools {
  McpAppTools._(this._handle, this._callable, this._allocations, this._tools,
      this._prefix);

  final ffi.Pointer<ffi.Void> _handle;
  final ffi.NativeCallable<_InvokeNative> _callable;
  final List<ffi.Pointer<ffi.NativeType>> _allocations;
  final Map<String, McpTool> _tools;
  final String _prefix;
  bool _released = false;

  /// Registers [tools] under [prefix] (a bare identifier prefix, `hvac_`).
  ///
  /// Throws [StateError] if the prefix is already claimed by another
  /// application or by the shell's own tools, and [ArgumentError] for a
  /// malformed registration.
  /// Reads the view this application is running as out of the entrypoint
  /// arguments the shell passes to `main`, or null when it is not there.
  ///
  /// An application cannot work this out for itself -- every instance of a
  /// bundle runs the same code -- and it matters as soon as two instances run
  /// at once, because both would otherwise claim the same tool namespace and
  /// the second would lose. Pass the result to [register] as `view`.
  ///
  /// The same name addresses this view's semantics tree, so an agent reading a
  /// tree and calling a tool sees one identity.
  static String? viewFromArgs(List<String> args) {
    const String flag = '--ihs-view=';
    for (final String arg in args) {
      if (arg.startsWith(flag)) {
        final String value = arg.substring(flag.length);
        return value.isEmpty ? null : value;
      }
    }
    return null;
  }

  /// The prefix the tools are actually advertised under.
  ///
  /// Usually [prefix] as asked for. When another application had already
  /// claimed it and a [view] was given, it is that prefix qualified by the
  /// view -- so the tools are reachable, under a name that says which view
  /// they act on. Worth logging: it is the difference between tools an agent
  /// can find and tools that are there under another name.
  String get prefix => _prefix;

  static McpAppTools register({
    required String prefix,
    required List<McpTool> tools,
    String? view,
    Duration timeout = const Duration(seconds: 5),
  }) {
    final Map<String, McpTool> byName = <String, McpTool>{
      for (final McpTool tool in tools) tool.name: tool,
    };
    if (byName.length != tools.length) {
      throw ArgumentError('two tools share a name; each must be unique');
    }

    // Held until unregister: the shell copies the strings during
    // registration, but the descriptor is read within the call and the
    // callback outlives it.
    final List<ffi.Pointer<ffi.NativeType>> allocations =
        <ffi.Pointer<ffi.NativeType>>[];

    ffi.Pointer<Utf8> allocate(String value) {
      final ffi.Pointer<Utf8> pointer = value.toNativeUtf8();
      allocations.add(pointer);
      return pointer;
    }

    final ffi.Pointer<_AppTool> array = calloc<_AppTool>(tools.length);
    allocations.add(array);
    for (int i = 0; i < tools.length; i++) {
      final McpTool tool = tools[i];
      array[i]
        ..name = allocate(tool.name)
        ..description = allocate(tool.description)
        ..inputSchemaJson = allocate(jsonEncode(tool.inputSchema))
        ..capability = 0; // the shell's default: an interacting tool
    }

    // A listener rather than an isolateLocal callback: the shell invokes this
    // from the thread serving the request, not from the isolate, and a
    // listener is the form that is safe to call from any thread. It also
    // returns immediately, which is what lets the handler be asynchronous.
    late final ffi.NativeCallable<_InvokeNative> callable;
    callable = ffi.NativeCallable<_InvokeNative>.listener(_onInvoke);

    final ffi.Pointer<_AppToolsDesc> desc = calloc<_AppToolsDesc>();
    allocations.add(desc);
    desc.ref
      ..structSize = ffi.sizeOf<_AppToolsDesc>()
      ..toolPrefix = allocate(prefix)
      ..tools = array
      ..toolCount = tools.length
      ..invoke = callable.nativeFunction
      ..userData = ffi.nullptr
      ..callTimeoutMs = timeout.inMilliseconds
      ..view = view == null ? ffi.nullptr : allocate(view);

    final ffi.Pointer<ffi.Pointer<ffi.Void>> out = calloc<ffi.Pointer<ffi.Void>>();
    final int status = _abi.register(desc, out);
    final ffi.Pointer<ffi.Void> handle = out.value;
    calloc.free(out);

    if (status != _ok) {
      callable.close();
      for (final ffi.Pointer<ffi.NativeType> pointer in allocations) {
        calloc.free(pointer);
      }
      if (status == _errPrefixTaken) {
        throw StateError(
            "the tool prefix '$prefix' is already claimed; pick another");
      }
      throw ArgumentError('registration was refused (status $status)');
    }

    // Read back rather than assumed: the shell may have qualified it, and an
    // application that logs the prefix it asked for would be reporting a name
    // no agent will see.
    final String effective = _abi.prefixOf(handle).toDartString();
    final McpAppTools registration =
        McpAppTools._(handle, callable, allocations, byName, effective);
    _live[callable.nativeFunction.address] = registration;
    return registration;
  }

  /// Withdraws the tools. Calls already in flight are failed rather than left
  /// to time out. Safe to call more than once.
  void unregister() {
    if (_released) {
      return;
    }
    _released = true;
    _live.remove(_callable.nativeFunction.address);
    // Order matters: the shell drains anything in flight and guarantees no
    // further invoke before this returns, so the callback is only safe to
    // close afterwards.
    _abi.unregister(_handle);
    _callable.close();
    for (final ffi.Pointer<ffi.NativeType> pointer in _allocations) {
      calloc.free(pointer);
    }
  }
}

// There is one registration per callback, and the callback is a bare C
// function pointer with no closure -- so the invoke is matched back to its
// registration by that pointer.
final Map<int, McpAppTools> _live = <int, McpAppTools>{};

void _onInvoke(
  ffi.Pointer<ffi.Void> userData,
  int callId,
  ffi.Pointer<Utf8> toolName,
  ffi.Pointer<Utf8> argumentsJson,
  int argumentsLength,
) {
  final String name = toolName.toDartString();
  final String rawArguments =
      argumentsLength == 0 ? '{}' : argumentsJson.toDartString(length: argumentsLength);

  // Exactly one registration can own a given tool name at a time, because the
  // shell refuses an overlapping prefix.
  McpTool? tool;
  for (final McpAppTools registration in _live.values) {
    tool = registration._tools[name];
    if (tool != null) {
      break;
    }
  }
  if (tool == null) {
    _complete(callId, false, <String, Object?>{'error': 'no such tool: $name'});
    return;
  }

  Map<String, Object?> arguments;
  try {
    final Object? decoded = jsonDecode(rawArguments);
    arguments = decoded is Map<String, Object?> ? decoded : <String, Object?>{};
  } on FormatException catch (error) {
    _complete(callId, false,
        <String, Object?>{'error': 'arguments were not JSON: $error'});
    return;
  }

  // Run through a Future even when the handler is synchronous, so a throw is
  // reported the same way in both cases rather than escaping into the
  // isolate's error handler and leaving the call to time out.
  Future<void>(() async => tool!.handler(arguments)).then(
    (Object? result) => _complete(callId, true, result),
    onError: (Object error) =>
        _complete(callId, false, <String, Object?>{'error': '$error'}),
  );
}

void _complete(int callId, bool ok, Object? result) {
  if (result == null) {
    _abi.complete(callId, ok, ffi.nullptr, 0);
    return;
  }
  final String encoded = jsonEncode(result);
  final ffi.Pointer<Utf8> buffer = encoded.toNativeUtf8();
  try {
    _abi.complete(callId, ok, buffer, encoded.length);
  } finally {
    // The shell copies before returning, so this is done with either way.
    calloc.free(buffer);
  }
}
