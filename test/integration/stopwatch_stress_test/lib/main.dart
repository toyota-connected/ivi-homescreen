// Integration test app for the ivi-homescreen watchdog under system stress.
//
// This app is the Flutter workload half of the stress_watchdog integration
// test (scripts/stress_watchdog_integration.sh). The embedder is built with
// -DBUILD_WATCHDOG=ON and -DBUILD_SYSTEMD_WATCHDOG=ON. While this app runs a
// stopwatch and animates a spinner, the test script runs stress-ng in parallel
// to load the CPU/memory. If the main or render thread is starved past the
// watchdog timeout, the embedder aborts (SIGABRT). The test passes when the
// app keeps running (petting the watchdog implicitly by staying responsive)
// for the configured duration.
//
// Behaviour:
//   * Displays a stopwatch showing how long the app has been running.
//   * Shows an animated CircularProgressIndicator spinner.
//   * Appends the current uptime to a file once every minute.
//   * Registers a watchdog source via the 'watchdog' MethodChannel and pets it
//     on a timer. A "Stop petting watchdog" button cancels the pet timer,
//     which starves the source and provokes the embedder to abort (SIGABRT)
//     once the watchdog timeout elapses.
//
// The uptime file path comes from the STOPWATCH_UPTIME_FILE environment
// variable (set by the test script) and falls back to
// /tmp/stopwatch_stress_uptime.txt.
//
// CI output (printed to stdout):
//   STOPWATCH_STRESS_START           — printed once at startup
//   STOPWATCH_STRESS_TICK <seconds>  — printed every minute after writing file
//   STOPWATCH_STRESS_WRITE_FAIL <e>  — printed if the file write throws
//   STOPWATCH_STRESS_STOP_PET        — printed when the pet timer is cancelled

import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

// ---------------------------------------------------------------------------
// Watchdog channel
// ---------------------------------------------------------------------------

// The embedder-provided watchdog MethodChannel (present when built with
// -DBUILD_WATCHDOG=ON). This app registers its own source and pets it on a
// timer so that cancelling the timer starves the source and forces a crash.
const MethodChannel _watchdogChannel = MethodChannel('watchdog');

// Source ID used for this app's watchdog registration. Must be outside the
// embedder-reserved range (0-2 and the negative main/render IDs).
const int _watchdogSource = 7;

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void main() {
  FlutterError.onError = (FlutterErrorDetails details) {
    FlutterError.dumpErrorToConsole(details);
    if (details.stack != null) {
      debugPrint('STACK:\n${details.stack}');
    }
  };
  print('STOPWATCH_STRESS_START');
  runApp(const StopwatchStressApp());
}

// ---------------------------------------------------------------------------
// Uptime file helper
// ---------------------------------------------------------------------------

String _uptimeFilePath() {
  final fromEnv = Platform.environment['STOPWATCH_UPTIME_FILE'];
  if (fromEnv != null && fromEnv.isNotEmpty) {
    return fromEnv;
  }
  return '/tmp/stopwatch_stress_uptime.txt';
}

// ---------------------------------------------------------------------------
// App shell
// ---------------------------------------------------------------------------

class StopwatchStressApp extends StatelessWidget {
  const StopwatchStressApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Stopwatch Stress Test',
      theme: ThemeData(
        colorSchemeSeed: Colors.indigo,
        brightness: Brightness.dark,
        useMaterial3: true,
      ),
      home: const _Home(),
    );
  }
}

// ---------------------------------------------------------------------------
// Home widget
// ---------------------------------------------------------------------------

class _Home extends StatefulWidget {
  const _Home();

  @override
  State<_Home> createState() => _HomeState();
}

class _HomeState extends State<_Home> {
  final Stopwatch _stopwatch = Stopwatch();
  final String _uptimeFile = _uptimeFilePath();

  Timer? _uiTimer;
  Timer? _writeTimer;
  Timer? _petTimer;
  Duration _elapsed = Duration.zero;
  int _writeCount = 0;
  String _lastWrite = '(none yet)';
  bool _petting = false;

  @override
  void initState() {
    super.initState();
    _stopwatch.start();

    // Truncate the uptime file at startup so a stale file from a previous run
    // does not confuse the test verifier.
    try {
      File(_uptimeFile).writeAsStringSync('');
    } catch (e) {
      print('STOPWATCH_STRESS_WRITE_FAIL $e');
    }

    // UI refresh — 10 Hz keeps the stopwatch smooth without busy-spinning.
    _uiTimer = Timer.periodic(const Duration(milliseconds: 100), (_) {
      setState(() => _elapsed = _stopwatch.elapsed);
    });

    // Write uptime to the file once every minute.
    _writeTimer = Timer.periodic(const Duration(minutes: 1), (_) {
      _writeUptime();
    });

    // Register a watchdog source and start petting it. Cancelling _petTimer
    // (via the button) starves the source and provokes an abort().
    _startWatchdogPetting();
  }

  @override
  void dispose() {
    _uiTimer?.cancel();
    _writeTimer?.cancel();
    _petTimer?.cancel();
    _stopwatch.stop();
    super.dispose();
  }

  Future<void> _startWatchdogPetting() async {
    try {
      await _watchdogChannel.invokeMethod<void>(
        'start',
        {'source': _watchdogSource, 'name': 'StopwatchStressApp'},
      );
    } catch (e) {
      // Channel absent (BUILD_WATCHDOG=OFF) or start failed — the button will
      // then have no effect, but the rest of the app still runs.
      print('STOPWATCH_STRESS_WATCHDOG_UNAVAILABLE $e');
      return;
    }

    // Pet well within the watchdog timeout window. A 1 s cadence stays under
    // the default 5 s timeout with generous headroom.
    _petTimer = Timer.periodic(const Duration(seconds: 1), (_) async {
      try {
        await _watchdogChannel
            .invokeMethod<void>('pet', {'source': _watchdogSource});
      } catch (_) {
        // Ignore transient pet failures.
      }
    });
    setState(() => _petting = true);
  }

  // Stops petting the watchdog source. The source is left registered but never
  // pet again, so the embedder's watchdog thread fires after the timeout and
  // aborts the process (SIGABRT).
  void _stopWatchdogPetting() {
    _petTimer?.cancel();
    _petTimer = null;
    print('STOPWATCH_STRESS_STOP_PET');
    setState(() => _petting = false);
  }

  void _writeUptime() {
    final int seconds = _stopwatch.elapsed.inSeconds;
    final String line = '${DateTime.now().toIso8601String()} '
        'uptime_seconds=$seconds\n';
    try {
      File(_uptimeFile).writeAsStringSync(line, mode: FileMode.append);
      _writeCount++;
      setState(() => _lastWrite = line.trim());
      print('STOPWATCH_STRESS_TICK $seconds');
    } catch (e) {
      print('STOPWATCH_STRESS_WRITE_FAIL $e');
    }
  }

  String _format(Duration d) {
    final h = d.inHours.toString().padLeft(2, '0');
    final m = (d.inMinutes % 60).toString().padLeft(2, '0');
    final s = (d.inSeconds % 60).toString().padLeft(2, '0');
    final ms = (d.inMilliseconds % 1000).toString().padLeft(3, '0');
    return '$h:$m:$s.$ms';
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Stopwatch Stress Test')),
      body: Center(
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            const SizedBox(
              width: 96,
              height: 96,
              child: CircularProgressIndicator(strokeWidth: 6),
            ),
            const SizedBox(height: 40),
            Text(
              _format(_elapsed),
              style: Theme.of(context).textTheme.displayMedium?.copyWith(
                    fontFeatures: const [FontFeature.tabularFigures()],
                    fontWeight: FontWeight.bold,
                  ),
            ),
            const SizedBox(height: 8),
            Text(
              'uptime',
              style: Theme.of(context).textTheme.titleMedium?.copyWith(
                    color: Colors.white54,
                  ),
            ),
            const SizedBox(height: 40),
            Text(
              'writes: $_writeCount',
              style: Theme.of(context).textTheme.bodyMedium,
            ),
            const SizedBox(height: 4),
            Text(
              'file: $_uptimeFile',
              style: Theme.of(context).textTheme.bodySmall?.copyWith(
                    color: Colors.white38,
                  ),
            ),
            const SizedBox(height: 4),
            Text(
              'last: $_lastWrite',
              style: Theme.of(context).textTheme.bodySmall?.copyWith(
                    color: Colors.white38,
                  ),
            ),
            const SizedBox(height: 40),
            FilledButton.icon(
              onPressed: _petting ? _stopWatchdogPetting : null,
              icon: const Icon(Icons.dangerous),
              label: Text(
                _petting
                    ? 'Stop petting watchdog (crash)'
                    : 'Watchdog no longer petted',
              ),
              style: FilledButton.styleFrom(
                backgroundColor: const Color(0xFFF44336),
                foregroundColor: Colors.white,
                padding: const EdgeInsets.symmetric(
                  horizontal: 24,
                  vertical: 16,
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}
