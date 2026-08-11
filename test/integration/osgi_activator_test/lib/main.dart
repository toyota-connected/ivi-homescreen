// Minimal OSGi bundle activator for ivi-homescreen.
//
// This is the smallest thing that makes a bundle a *bundle* rather than an
// ordinary Flutter app: it completes the `dev.osgi/bridge` handshake and then
// declares itself ACTIVE.
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
//   1. init    hands over the two things native code cannot obtain by itself:
//              the address of NativeApi.initializeApiDLData (so the shell can
//              bind Dart_PostCObject_DL) and this isolate's receive port. The
//              shell replies once it has recorded them, and posts the framework
//              isolate's port back to that receive port whenever it knows it --
//              which may be before or after this call returns.
//   2. active  sent after the activator's own start-up work finishes. Here that
//              work is trivial; in a real bundle it is whatever must be true
//              before the bundle is fit to be seen.
//
// Symbolic name comes from --dart-entrypoint-args so one build can stand in for
// any bundle in a config; it must match the [[osgi.bundles]] entry, because the
// shell refuses a report from a name it does not know.

// NativeApi and SendPort.nativePort both come from dart:ffi, not dart:ui.
import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

const MethodChannel _bridge = MethodChannel('dev.osgi/bridge');

/// Result of the handshake, surfaced on screen so a failure is visible on the
/// panel rather than only in the log.
enum _BundleState { starting, active, failed }

Future<void> main(List<String> args) async {
  WidgetsFlutterBinding.ensureInitialized();

  // Passed via [osgi.bundles.args] dart = [...]; falls back to a name that will
  // be rejected, which is a louder failure than silently reporting as some
  // other bundle.
  final String symbolicName =
      args.isNotEmpty ? args.first : 'com.ivi.unnamed-bundle';

  final _Activator activator = _Activator(symbolicName);
  runApp(_ActivatorApp(activator: activator));

  // Deliberately after runApp: the channel needs the binding running, and there
  // is no reason to hold up the first frame for the handshake.
  await activator.start();
}

class _Activator {
  _Activator(this.symbolicName);

  final String symbolicName;
  final ValueNotifier<_BundleState> state =
      ValueNotifier<_BundleState>(_BundleState.starting);
  final ValueNotifier<String> detail = ValueNotifier<String>('registering');

  /// Kept alive for the isolate's lifetime: the shell posts the framework
  /// isolate's port here, and closing it would strand that message.
  final ReceivePort _fromShell = ReceivePort();

  int? frameworkPort;

  /// Repaints steadily so the bundle keeps presenting.
  ///
  /// Not decoration: a Flutter tree with nothing changing produces no damage
  /// and therefore no frames, so a static bundle stops page-flipping within a
  /// few frames of start-up. The harness proves both CRTCs are live by counting
  /// flips, and a bundle that has legitimately gone idle looks identical to one
  /// that never presented at all. A real cluster or navigation view animates;
  /// this stands in for that.
  final ValueNotifier<int> tick = ValueNotifier<int>(0);
  Timer? _ticker;

  Future<void> start() async {
    _ticker = Timer.periodic(const Duration(milliseconds: 100),
        (_) => tick.value = tick.value + 1);

    _fromShell.listen((dynamic message) {
      // The framework port arrives as a bare int (Dart_PostCObject_DL with an
      // int64 payload). Everything after this is SendPort traffic between
      // isolates, off the platform thread.
      if (message is int) {
        frameworkPort = message;
        detail.value = 'framework port $message';
      }
    });

    try {
      await _bridge.invokeMethod<bool>('init', <String, dynamic>{
        'role': 'bundle',
        'symbolic_name': symbolicName,
        // The shell binds its Dart DL symbol table from this. Every isolate
        // sends it because any of them may be the first to arrive; the shell
        // makes all but the first a no-op.
        'dl_data': NativeApi.initializeApiDLData.address,
        'port': _fromShell.sendPort.nativePort,
      });
    } on PlatformException catch (e) {
      // Most likely a symbolic_name with no matching [[osgi.bundles]] entry, or
      // a shell built without ENABLE_OSGI.
      state.value = _BundleState.failed;
      detail.value = 'init rejected: ${e.code} ${e.message ?? ''}';
      return;
    } on MissingPluginException {
      state.value = _BundleState.failed;
      detail.value = 'no dev.osgi/bridge (shell built without ENABLE_OSGI?)';
      return;
    }

    // Whatever has to be true before this bundle is fit to be seen would go
    // here. Reporting ACTIVE before that is done would defeat the guarantee the
    // shell is holding the reactor for.
    await _startupWork();

    try {
      await _bridge.invokeMethod<bool>('active', <String, dynamic>{
        'symbolic_name': symbolicName,
      });
      state.value = _BundleState.active;
      detail.value = frameworkPort == null
          ? 'ACTIVE (awaiting framework port)'
          : 'ACTIVE (framework port $frameworkPort)';
    } on PlatformException catch (e) {
      state.value = _BundleState.failed;
      detail.value = 'active rejected: ${e.code}';
    }
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
