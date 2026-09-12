// Minimal OSGi bundle activator for ivi-homescreen.
//
// This is the smallest thing that makes a bundle a *bundle* rather than an
// ordinary Flutter app: it completes the shell's handshake and then declares
// itself ACTIVE.
//
// That declaration is the whole point. A critical bundle's startup wait is
// released by ACTIVE and nothing else -- not by the engine coming up, which the
// shell already knows about, and not by the first frame, which says nothing
// about whether the bundle's own code is ready. Until some bundle sends it, the
// shell can only ever time out a critical bundle, which is why the multi-bundle
// harness cannot assert critical-first ordering without an app like this one.
//
// The handshake, in order:
//
//   1. register  hands over the two things native code cannot obtain by
//                itself: the address of NativeApi.initializeApiDLData (so the
//                shell can bind Dart_PostCObject_DL) and this isolate's receive
//                port. The shell posts the framework isolate's port back to
//                that receive port whenever it knows it -- which may be before
//                or after this call returns.
//   2. active    sent after the activator's own start-up work finishes. Here
//                that work is trivial; in a real bundle it is whatever must be
//                true before the bundle is fit to be seen.
//
// TWO TRANSPORTS, ONE APP
//
// The same handshake travels either over the dev.osgi/bridge MethodChannel or
// over the ihs_osgi_* C ABI in libihs_shared, chosen by the second Dart
// entrypoint argument. One app rather than two because a bundle directory must
// not ship its own lib/libflutter_engine.so -- LibFlutterEngine::Load is
// one-shot and keyed on the path it first binds -- so a second activator bundle
// would collide with this one for reasons that have nothing to do with what is
// being tested.
//
// The FFI path still runs a widget tree and still repaints. The point of it is
// "no channel, no platform thread", not headlessness: the harness proves both
// CRTCs are live by counting page flips, so a bundle that stopped presenting
// would take B5 with it.
//
// Symbolic name comes from --dart-entrypoint-args so one build can stand in for
// any bundle in a config; it must match the [[osgi.bundles]] entry, because the
// shell refuses a registration naming anything the configuration does not
// declare.

// NativeApi and SendPort.nativePort both come from dart:ffi, not dart:ui.
//
// Imported twice, unprefixed and as `ffi`, on purpose: dart:ui exports a Size
// of its own (the geometry one) through package:flutter/widgets.dart, so a bare
// @Size() on a struct field resolves to that and fails as "not a const
// annotation". The prefix is only needed where the two collide.
import 'dart:async';
import 'dart:ffi';
import 'dart:ffi' as ffi;
import 'dart:isolate';

import 'package:ffi/ffi.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

/// Result of the handshake, surfaced on screen so a failure is visible on the
/// panel rather than only in the log.
enum _BundleState { starting, active, failed }

/// Raised by a transport when the shell refuses or cannot be reached. The
/// message is what lands on the panel.
class _HandshakeFailure implements Exception {
  _HandshakeFailure(this.message);

  final String message;

  @override
  String toString() => message;
}

/// How this bundle talks to the shell.
///
/// The seam exists for the same reason the Dart framework's `ShellTransport`
/// does: which transport a bundle gets is a property of how far it is trusted
/// -- `dart:ffi` grants arbitrary access to the process address space -- not of
/// what the bundle needs. The activator's own code is identical either way.
abstract class _Transport {
  String get name;

  /// Announce this bundle. [replyTo] is where the shell posts the framework
  /// isolate's port.
  Future<void> register(String symbolicName, SendPort replyTo);

  /// Report that start-up finished. This is what releases the critical wait.
  Future<void> reportActive(String symbolicName);
}

// ---------------------------------------------------------------------------
// MethodChannel
// ---------------------------------------------------------------------------

const MethodChannel _bridge = MethodChannel('dev.osgi/bridge');

/// The default. Proven on hardware, and needs nothing of the shell that is not
/// already there -- at the cost of a UI binding even for a headless bundle, and
/// an ACTIVE report marshalled through the platform task runner during the
/// busiest part of start-up, while the startup deadline is running.
class _ChannelTransport implements _Transport {
  @override
  String get name => 'channel';

  @override
  Future<void> register(String symbolicName, SendPort replyTo) async {
    try {
      await _bridge.invokeMethod<bool>('init', <String, dynamic>{
        'role': 'bundle',
        'symbolic_name': symbolicName,
        // The shell binds its Dart DL symbol table from this. Every isolate
        // sends it because any of them may be the first to arrive; the shell
        // makes all but the first a no-op.
        'dl_data': NativeApi.initializeApiDLData.address,
        'port': replyTo.nativePort,
      });
    } on PlatformException catch (e) {
      // Most likely a symbolic_name with no matching [[osgi.bundles]] entry, or
      // a shell built without ENABLE_OSGI.
      throw _HandshakeFailure('init rejected: ${e.code} ${e.message ?? ''}');
    } on MissingPluginException {
      throw _HandshakeFailure(
          'no dev.osgi/bridge (shell built without ENABLE_OSGI?)');
    }
  }

  @override
  Future<void> reportActive(String symbolicName) async {
    try {
      await _bridge.invokeMethod<bool>('active', <String, dynamic>{
        'symbolic_name': symbolicName,
      });
    } on PlatformException catch (e) {
      throw _HandshakeFailure('active rejected: ${e.code}');
    } on MissingPluginException {
      throw _HandshakeFailure('no dev.osgi/bridge');
    }
  }
}

// ---------------------------------------------------------------------------
// FFI
// ---------------------------------------------------------------------------

/// `IhsOsgiStatus`. Negative values are errors.
const int _ihsOsgiOk = 0;
const int _ihsOsgiInvalid = -1;
const int _ihsOsgiDartApi = -2;
const int _ihsOsgiRejected = -3;
const int _ihsOsgiUnavailable = -4;

String _statusName(int status) => switch (status) {
      _ihsOsgiOk => 'OK',
      _ihsOsgiInvalid => 'INVALID',
      _ihsOsgiDartApi => 'DART_API',
      _ihsOsgiRejected => 'REJECTED',
      _ihsOsgiUnavailable => 'UNAVAILABLE',
      _ => 'status $status',
    };

/// `IhsOsgiPeerInfo`.
final class _PeerInfo extends Struct {
  @ffi.Size()
  external int structSize;

  external Pointer<Void> dartApiDlData;

  @Int64()
  external int port;
}

/// `IhsOsgiBundleInfo`.
final class _BundleInfo extends Struct {
  @ffi.Size()
  external int structSize;

  external _PeerInfo peer;

  external Pointer<Utf8> symbolicName;
}

typedef _RegisterBundleNative = Int32 Function(
    Pointer<_BundleInfo>, Pointer<Pointer<Void>>);
typedef _RegisterBundleDart = int Function(
    Pointer<_BundleInfo>, Pointer<Pointer<Void>>);
typedef _ReportNative = Int32 Function(Pointer<Void>);
typedef _ReportDart = int Function(Pointer<Void>);

/// The same handshake through `libihs_shared`: synchronous, no UI binding
/// required, and never routed through the platform task runner.
///
/// Bindings are hand-rolled rather than taken from the `osgi_ffi` package, which
/// lives in another repository: this fixture stays self-contained, exactly as
/// the channel path does not depend on `osgi_flutter`.
class _FfiTransport implements _Transport {
  _FfiTransport._(this._registerBundle, this._reportActive);

  /// The versioned SONAME, which is the open string PLUGIN_ABI.md documents.
  /// The shell links this library, so it is already resident and this resolves
  /// the copy already in the process.
  static const String _library = 'libihs_shared.so.1';

  /// Bind the surface, or explain why it is not there.
  ///
  /// A shell built without ENABLE_OSGI exports no ihs_osgi_* symbol at all, so
  /// a missing symbol is the ordinary "no OSGi here" answer rather than a
  /// broken installation -- and saying which it is beats a bare lookup failure
  /// on the panel.
  factory _FfiTransport.open() {
    final DynamicLibrary library;
    try {
      library = DynamicLibrary.open(_library);
    } on ArgumentError catch (e) {
      throw _HandshakeFailure('cannot open $_library: $e');
    }
    try {
      return _FfiTransport._(
        library.lookupFunction<_RegisterBundleNative, _RegisterBundleDart>(
            'ihs_osgi_register_bundle'),
        library.lookupFunction<_ReportNative, _ReportDart>(
            'ihs_osgi_report_active'),
      );
    } on ArgumentError {
      throw _HandshakeFailure(
          '$_library has no ihs_osgi_* symbols (ENABLE_OSGI off?)');
    }
  }

  final _RegisterBundleDart _registerBundle;
  final _ReportDart _reportActive;

  /// The capability the shell mints at registration. Opaque: it is never
  /// dereferenced here, and reporting ACTIVE needs it rather than a name,
  /// because any code in the process can reach these symbols and a name would
  /// be no barrier at all.
  Pointer<Void> _handle = nullptr;

  @override
  String get name => 'ffi';

  @override
  Future<void> register(String symbolicName, SendPort replyTo) async {
    final Pointer<_BundleInfo> info = calloc<_BundleInfo>();
    final Pointer<Pointer<Void>> outBundle = calloc<Pointer<Void>>();
    final Pointer<Utf8> name = symbolicName.toNativeUtf8();
    try {
      info.ref.structSize = sizeOf<_BundleInfo>();
      info.ref.peer.structSize = sizeOf<_PeerInfo>();
      info.ref.peer.dartApiDlData = NativeApi.initializeApiDLData;
      info.ref.peer.port = replyTo.nativePort;
      info.ref.symbolicName = name;

      final int status = _registerBundle(info, outBundle);
      if (status != _ihsOsgiOk) {
        throw _HandshakeFailure('register rejected: ${_statusName(status)}');
      }
      _handle = outBundle.value;
      if (_handle == nullptr) {
        // Accepted with nothing to report through: every later call would fail
        // with no explanation, so refuse it here instead.
        throw _HandshakeFailure('register returned no handle');
      }
    } finally {
      // The shell copies the name, so it need not outlive the call.
      calloc.free(name);
      calloc.free(outBundle);
      calloc.free(info);
    }
  }

  @override
  Future<void> reportActive(String symbolicName) async {
    if (_handle == nullptr) {
      throw _HandshakeFailure('no registration to report ACTIVE for');
    }
    final int status = _reportActive(_handle);
    if (status != _ihsOsgiOk) {
      throw _HandshakeFailure('active rejected: ${_statusName(status)}');
    }
  }
}

// ---------------------------------------------------------------------------

Future<void> main(List<String> args) async {
  WidgetsFlutterBinding.ensureInitialized();

  // Passed via [osgi.bundles.args] dart = [...]; falls back to a name that will
  // be rejected, which is a louder failure than silently reporting as some
  // other bundle.
  final String symbolicName =
      args.isNotEmpty ? args.first : 'com.ivi.unnamed-bundle';
  final String transportName = args.length > 1 ? args[1] : 'channel';

  final _Activator activator = _Activator(symbolicName, transportName);
  runApp(_ActivatorApp(activator: activator));

  // Deliberately after runApp: the channel needs the binding running, and there
  // is no reason to hold up the first frame for the handshake.
  await activator.start();
}

class _Activator {
  _Activator(this.symbolicName, this.transportName);

  final String symbolicName;
  final String transportName;
  final ValueNotifier<_BundleState> state =
      ValueNotifier<_BundleState>(_BundleState.starting);
  final ValueNotifier<String> detail = ValueNotifier<String>('registering');

  /// Kept alive for the isolate's lifetime: the shell posts the framework
  /// isolate's port here, and closing it would strand that message.
  final ReceivePort _fromShell = ReceivePort();

  /// The framework isolate's port, once the shell posts it.
  ///
  /// A [SendPort] rather than an int: a port id would be useless here, since
  /// Dart offers no way to turn one back into a [SendPort].
  SendPort? frameworkPort;

  /// Repaints steadily so the bundle keeps presenting.
  ///
  /// Not decoration: a Flutter tree with nothing changing produces no damage
  /// and therefore no frames, so a static bundle stops page-flipping within a
  /// few frames of start-up. The harness proves both CRTCs are live by counting
  /// flips, and a bundle that has legitimately gone idle looks identical to one
  /// that never presented at all. A real cluster or navigation view animates;
  /// this stands in for that.
  final ValueNotifier<int> tick = ValueNotifier<int>(0);

  Future<void> start() async {
    // Never cancelled, deliberately: the bundle presents for as long as its
    // isolate lives, and there is no teardown path here to cancel it from.
    Timer.periodic(
        const Duration(milliseconds: 100), (_) => tick.value = tick.value + 1);

    _fromShell.listen((dynamic message) {
      // Arrives as a SendPort, because the shell posts a
      // Dart_CObject_kSendPort. An int would be useless here: Dart cannot turn
      // a port id back into a SendPort, so a bundle handed one has nothing to
      // send to.
      //
      // Everything after this is SendPort traffic between isolates, off the
      // platform thread -- and the same on both transports, since only the
      // bootstrap differs.
      if (message is SendPort) {
        frameworkPort = message;
        detail.value = 'framework port received';
      }
    });

    final _Transport transport;
    try {
      transport = switch (transportName) {
        'ffi' => _FfiTransport.open(),
        'channel' => _ChannelTransport(),
        _ => throw _HandshakeFailure(
            "unknown transport '$transportName' (expected channel|ffi)"),
      };
    } on _HandshakeFailure catch (e) {
      _fail(e.message);
      return;
    }

    try {
      await transport.register(symbolicName, _fromShell.sendPort);
    } on _HandshakeFailure catch (e) {
      _fail(e.message);
      return;
    }

    // Whatever has to be true before this bundle is fit to be seen would go
    // here. Reporting ACTIVE before that is done would defeat the guarantee the
    // shell is holding the reactor for.
    await _startupWork();

    try {
      await transport.reportActive(symbolicName);
    } on _HandshakeFailure catch (e) {
      _fail(e.message);
      return;
    }

    state.value = _BundleState.active;
    detail.value = frameworkPort == null
        ? 'ACTIVE (awaiting framework port)'
        : 'ACTIVE (framework port received)';
  }

  void _fail(String why) {
    state.value = _BundleState.failed;
    detail.value = why;
  }

  Future<void> _startupWork() async {
    // Nothing real to do; yield once so ACTIVE is genuinely sent after start-up
    // completes rather than in the same microtask, which is the ordering a real
    // activator has.
    await Future<void>.delayed(Duration.zero);
  }
}

/// Shows the bundle name and handshake state. Color is the fastest read on a
/// panel: amber while starting, green once ACTIVE, red if the handshake failed.
class _ActivatorApp extends StatelessWidget {
  const _ActivatorApp({required this.activator});

  final _Activator activator;

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<_BundleState>(
      valueListenable: activator.state,
      builder: (BuildContext context, _BundleState state, _) {
        final Color background = switch (state) {
          _BundleState.starting => const Color(0xFF7A5C00),
          _BundleState.active => const Color(0xFF0B5D1E),
          _BundleState.failed => const Color(0xFF7A0000),
        };
        return Directionality(
          textDirection: TextDirection.ltr,
          child: ColoredBox(
            color: background,
            child: Center(
              child: ValueListenableBuilder<String>(
                valueListenable: activator.detail,
                builder: (BuildContext context, String detail, _) {
                  return Column(
                    mainAxisAlignment: MainAxisAlignment.center,
                    children: <Widget>[
                      Text(
                        activator.symbolicName,
                        style: const TextStyle(
                          color: Color(0xFFFFFFFF),
                          fontSize: 40,
                          fontWeight: FontWeight.bold,
                        ),
                      ),
                      const SizedBox(height: 16),
                      Text(
                        state.name.toUpperCase(),
                        style: const TextStyle(
                          color: Color(0xFFFFFFFF),
                          fontSize: 64,
                          fontWeight: FontWeight.bold,
                        ),
                      ),
                      const SizedBox(height: 16),
                      Text(
                        detail,
                        style: const TextStyle(
                          color: Color(0xCCFFFFFF),
                          fontSize: 22,
                        ),
                      ),
                      const SizedBox(height: 8),
                      // Which path actually ran, so a photograph of the panel
                      // says so without cross-referencing the log.
                      Text(
                        'via ${activator.transportName}',
                        style: const TextStyle(
                          color: Color(0xCCFFFFFF),
                          fontSize: 20,
                        ),
                      ),
                      const SizedBox(height: 12),
                      // Visible proof the view is still presenting rather than
                      // parked on a stale frame.
                      ValueListenableBuilder<int>(
                        valueListenable: activator.tick,
                        builder: (BuildContext context, int tick, _) => Text(
                          'tick $tick',
                          style: const TextStyle(
                            color: Color(0x99FFFFFF),
                            fontSize: 18,
                          ),
                        ),
                      ),
                    ],
                  );
                },
              ),
            ),
          ),
        );
      },
    );
  }
}
