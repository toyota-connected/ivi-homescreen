// Integration test app for the ivi-homescreen watchdog platform channel.
//
// Tests the 'watchdog' MethodChannel exposed by WatchdogPlugin when the
// embedder is built with -DBUILD_WATCHDOG=ON.  No hardware interaction is
// required — the app exercises the channel directly and displays a per-check
// pass / fail / skip indicator.
//
// Checks performed (in order):
//   main_thread_alive     — waits 5.5 s; if the process is still running the
//                           main-thread watchdog has not fired
//   channel_available     — channel is reachable (no MissingPluginException)
//   start_stop            — start(3) then stop(3) succeed
//   pet_active            — start(3) -> pet(3) -> stop(3) succeed
//   pet_inactive_no_crash — pet(99) on unregistered source does not throw
//   stop_inactive_no_crash— stop(99) on unregistered source does not throw
//   multi_source          — start/pet/stop on two sources (3+4) concurrently
//   large_source_id       — start/pet/stop with a large source ID (1000000)
//   invalid_source_neg    — start(-1) returns PlatformException(invalid_source)
//   get_callbacks_shape   — get_callbacks returns a Map with start/pet/stop int keys
//   ffi_start_pet_stop    — cast pointers via dart:ffi and call start(5)/pet(5)/stop(5)
//
// If channel_available detects the channel is absent (MissingPluginException
// or 'unhandled_method' PlatformException), all remaining checks are marked
// SKIP and the verdict is WATCHDOG_TEST: SKIP.
//
// CI output (printed to stdout):
//   WATCHDOG_TEST_SUMMARY {"pass":N,"fail":N,"skip":N,"checks":{...}}
//   WATCHDOG_TEST: PASS | FAIL | SKIP

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

// ---------------------------------------------------------------------------
// FFI type aliases for watchdog native functions
// ---------------------------------------------------------------------------

typedef _WatchdogNative = Void Function(Int64);
typedef _WatchdogDart = void Function(int);

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
  runApp(const WatchdogTestApp());
}

// ---------------------------------------------------------------------------
// App shell
// ---------------------------------------------------------------------------

class WatchdogTestApp extends StatelessWidget {
  const WatchdogTestApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Watchdog Test',
      theme: ThemeData(
        colorSchemeSeed: Colors.teal,
        brightness: Brightness.dark,
        useMaterial3: true,
      ),
      home: const _Home(),
    );
  }
}

// ---------------------------------------------------------------------------
// Check model
// ---------------------------------------------------------------------------

enum CheckStatus { pending, pass, fail, skip }

class Check {
  Check(this.name);

  final String name;
  CheckStatus status = CheckStatus.pending;
  String detail = '';
}

// ---------------------------------------------------------------------------
// Channel helpers
// ---------------------------------------------------------------------------

const _channel = MethodChannel('watchdog');

// Returns true if the exception means the channel/method simply doesn't exist.
bool _isAbsent(Object e) {
  if (e is MissingPluginException) return true;
  if (e is PlatformException) {
    return e.code == 'unhandled_method' ||
        e.code == 'channel-error' ||
        e.message?.contains('No implementation found') == true;
  }
  return false;
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
  final List<Check> _checks = [
    Check('main_thread_alive'),
    Check('channel_available'),
    Check('start_stop'),
    Check('pet_active'),
    Check('pet_inactive_no_crash'),
    Check('stop_inactive_no_crash'),
    Check('multi_source'),
    Check('large_source_id'),
    Check('invalid_source_neg'),
    Check('get_callbacks_shape'),
    Check('ffi_start_pet_stop'),
  ];

  bool _running = false;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _runAll());
  }

  Check _check(String name) => _checks.firstWhere((c) => c.name == name);

  void _setCheck(String name, CheckStatus status, String detail) {
    setState(() {
      final c = _check(name);
      c.status = status;
      c.detail = detail;
    });
  }

  void _skipAll(String reason) {
    setState(() {
      for (final c in _checks) {
        if (c.status == CheckStatus.pending) {
          c.status = CheckStatus.skip;
          c.detail = reason;
        }
      }
    });
  }

  Future<void> _runAll() async {
    if (_running) return;
    _running = true;

    // Reset
    setState(() {
      for (final c in _checks) {
        c.status = CheckStatus.pending;
        c.detail = '';
      }
    });

    // ------------------------------------------------------------------
    // main_thread_alive
    // Wait 5.5 s. The default watchdog timeout is 5 s; if the process is
    // still alive the main thread has been petted successfully by the
    // embedder's loop.
    // ------------------------------------------------------------------
    _setCheck('main_thread_alive', CheckStatus.pending,
        'Waiting 5.5 s for watchdog timeout window…');
    await Future<void>.delayed(const Duration(milliseconds: 5500));
    _setCheck('main_thread_alive', CheckStatus.pass,
        'Process alive after 5.5 s — main-thread watchdog not triggered');

    // ------------------------------------------------------------------
    // channel_available
    // ------------------------------------------------------------------
    try {
      await _channel.invokeMethod<void>('start', {'source': 3});
      await _channel.invokeMethod<void>('stop', {'source': 3});
      _setCheck('channel_available', CheckStatus.pass, 'Channel reachable');
    } catch (e) {
      if (_isAbsent(e)) {
        _setCheck('channel_available', CheckStatus.skip,
            'Channel absent (BUILD_WATCHDOG=OFF?)');
        _skipAll('Channel absent — skipping remaining checks');
        _printSummary();
        _running = false;
        return;
      }
      _setCheck('channel_available', CheckStatus.pass,
          'Channel reachable (probe threw: $e)');
    }

    // ------------------------------------------------------------------
    // start_stop
    // ------------------------------------------------------------------
    try {
      await _channel.invokeMethod<void>('start', {'source': 3});
      await _channel.invokeMethod<void>('stop', {'source': 3});
      _setCheck('start_stop', CheckStatus.pass, 'start(3) → stop(3) succeeded');
    } catch (e) {
      _setCheck('start_stop', CheckStatus.fail, '$e');
    }

    // ------------------------------------------------------------------
    // pet_active
    // ------------------------------------------------------------------
    try {
      await _channel.invokeMethod<void>('start', {'source': 3});
      await _channel.invokeMethod<void>('pet', {'source': 3});
      await _channel.invokeMethod<void>('stop', {'source': 3});
      _setCheck('pet_active', CheckStatus.pass,
          'start(3) → pet(3) → stop(3) succeeded');
    } catch (e) {
      _setCheck('pet_active', CheckStatus.fail, '$e');
    }

    // ------------------------------------------------------------------
    // pet_inactive_no_crash
    // Embedder logs a warning but must not return an error envelope.
    // ------------------------------------------------------------------
    try {
      await _channel.invokeMethod<void>('pet', {'source': 99});
      _setCheck('pet_inactive_no_crash', CheckStatus.pass,
          'pet(99) on unregistered source did not throw');
    } catch (e) {
      if (e is PlatformException && e.code == 'invalid_source') {
        _setCheck('pet_inactive_no_crash', CheckStatus.pass,
            'pet(99) returned invalid_source (acceptable)');
      } else {
        _setCheck('pet_inactive_no_crash', CheckStatus.fail, '$e');
      }
    }

    // ------------------------------------------------------------------
    // stop_inactive_no_crash
    // ------------------------------------------------------------------
    try {
      await _channel.invokeMethod<void>('stop', {'source': 99});
      _setCheck('stop_inactive_no_crash', CheckStatus.pass,
          'stop(99) on unregistered source did not throw');
    } catch (e) {
      if (e is PlatformException && e.code == 'invalid_source') {
        _setCheck('stop_inactive_no_crash', CheckStatus.pass,
            'stop(99) returned invalid_source (acceptable)');
      } else {
        _setCheck('stop_inactive_no_crash', CheckStatus.fail, '$e');
      }
    }

    // ------------------------------------------------------------------
    // multi_source
    // ------------------------------------------------------------------
    try {
      await _channel.invokeMethod<void>('start', {'source': 3});
      await _channel.invokeMethod<void>('start', {'source': 4});
      await _channel.invokeMethod<void>('pet', {'source': 3});
      await _channel.invokeMethod<void>('pet', {'source': 4});
      await _channel.invokeMethod<void>('stop', {'source': 3});
      await _channel.invokeMethod<void>('stop', {'source': 4});
      _setCheck('multi_source', CheckStatus.pass,
          'Sources 3 and 4 started, petted, and stopped');
    } catch (e) {
      _setCheck('multi_source', CheckStatus.fail, '$e');
    }

    // ------------------------------------------------------------------
    // large_source_id
    // Verifies there is no arbitrary upper-bound restriction on source IDs.
    // ------------------------------------------------------------------
    try {
      await _channel.invokeMethod<void>('start', {'source': 1000000});
      await _channel.invokeMethod<void>('pet', {'source': 1000000});
      await _channel.invokeMethod<void>('stop', {'source': 1000000});
      _setCheck('large_source_id', CheckStatus.pass,
          'start/pet/stop(1000000) succeeded');
    } catch (e) {
      _setCheck('large_source_id', CheckStatus.fail, '$e');
    }

    // ------------------------------------------------------------------
    // invalid_source_neg
    // Negative source IDs must be rejected.
    // ------------------------------------------------------------------
    try {
      await _channel.invokeMethod<void>('start', {'source': -1});
      _setCheck('invalid_source_neg', CheckStatus.fail,
          'start(-1) should have returned invalid_source but succeeded');
    } catch (e) {
      if (e is PlatformException && e.code == 'invalid_source') {
        _setCheck('invalid_source_neg', CheckStatus.pass,
            'start(-1) rejected with invalid_source');
      } else {
        _setCheck('invalid_source_neg', CheckStatus.fail,
            'Expected PlatformException(invalid_source), got: $e');
      }
    }

    // ------------------------------------------------------------------
    // get_callbacks_shape
    // ------------------------------------------------------------------
    try {
      final callbacks =
          await _channel.invokeMethod<Map<Object?, Object?>>('get_callbacks');
      if (callbacks == null) {
        _setCheck('get_callbacks_shape', CheckStatus.fail,
            'get_callbacks returned null');
      } else {
        final start = callbacks['start'];
        final pet = callbacks['pet'];
        final stop = callbacks['stop'];
        if (start is int && start != 0 &&
            pet is int && pet != 0 &&
            stop is int && stop != 0) {
          _setCheck('get_callbacks_shape', CheckStatus.pass,
              'start=0x${start.toRadixString(16)}, '
              'pet=0x${pet.toRadixString(16)}, '
              'stop=0x${stop.toRadixString(16)}');
        } else {
          _setCheck('get_callbacks_shape', CheckStatus.fail,
              'Unexpected shape: start=$start pet=$pet stop=$stop');
        }
      }
    } catch (e) {
      _setCheck('get_callbacks_shape', CheckStatus.fail, '$e');
    }

    // ------------------------------------------------------------------
    // ffi_start_pet_stop
    // Use the function pointers returned by get_callbacks to call the
    // native watchdog functions directly via dart:ffi.
    // ------------------------------------------------------------------
    try {
      final callbacks =
          await _channel.invokeMethod<Map<Object?, Object?>>('get_callbacks');
      if (callbacks == null) {
        _setCheck('ffi_start_pet_stop', CheckStatus.fail,
            'get_callbacks returned null');
      } else {
        final startAddr = callbacks['start'] as int;
        final petAddr = callbacks['pet'] as int;
        final stopAddr = callbacks['stop'] as int;

        final nativeStart = Pointer<NativeFunction<_WatchdogNative>>
            .fromAddress(startAddr)
            .asFunction<_WatchdogDart>();
        final nativePet = Pointer<NativeFunction<_WatchdogNative>>
            .fromAddress(petAddr)
            .asFunction<_WatchdogDart>();
        final nativeStop = Pointer<NativeFunction<_WatchdogNative>>
            .fromAddress(stopAddr)
            .asFunction<_WatchdogDart>();

        nativeStart(5);
        nativePet(5);
        nativeStop(5);

        _setCheck('ffi_start_pet_stop', CheckStatus.pass,
            'FFI start(5) → pet(5) → stop(5) completed without crash');
      }
    } catch (e) {
      _setCheck('ffi_start_pet_stop', CheckStatus.fail, '$e');
    }

    _printSummary();
    _running = false;
  }

  void _printSummary() {
    final int passed =
        _checks.where((c) => c.status == CheckStatus.pass).length;
    final int failed =
        _checks.where((c) => c.status == CheckStatus.fail).length;
    final int skipped =
        _checks.where((c) => c.status == CheckStatus.skip).length;

    final Map<String, dynamic> checks = {
      for (final c in _checks)
        c.name: {'status': c.status.name, 'detail': c.detail},
    };

    final summary = jsonEncode({
      'pass': passed,
      'fail': failed,
      'skip': skipped,
      'checks': checks,
    });

    print('WATCHDOG_TEST_SUMMARY $summary');

    final String verdict;
    if (skipped == _checks.length) {
      verdict = 'SKIP';
    } else if (failed > 0) {
      verdict = 'FAIL';
    } else {
      verdict = 'PASS';
    }
    print('WATCHDOG_TEST: $verdict');

    // Wait 3 s so the UI result is visible, then exit with an appropriate code.
    // Exit code 0 = PASS or SKIP, exit code 1 = FAIL.
    Future<void>.delayed(const Duration(seconds: 3), () {
      exit(failed > 0 ? 1 : 0);
    });
  }

  // ---------------------------------------------------------------------------
  // UI
  // ---------------------------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    final int passed =
        _checks.where((c) => c.status == CheckStatus.pass).length;
    final int failed =
        _checks.where((c) => c.status == CheckStatus.fail).length;
    final int skipped =
        _checks.where((c) => c.status == CheckStatus.skip).length;

    return Scaffold(
      appBar: AppBar(
        title: const Text('Watchdog Channel Test'),
        actions: [
          IconButton(
            tooltip: 'Re-run all checks',
            icon: const Icon(Icons.refresh),
            onPressed: _running ? null : _runAll,
          ),
        ],
      ),
      body: ListView.separated(
        padding: const EdgeInsets.symmetric(vertical: 8),
        itemCount: _checks.length,
        separatorBuilder: (_, __) => const Divider(height: 1),
        itemBuilder: (context, i) => _CheckRow(check: _checks[i]),
      ),
      bottomNavigationBar: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Row(
            mainAxisAlignment: MainAxisAlignment.spaceEvenly,
            children: [
              _SummaryBadge(
                color: const Color(0xFF4CAF50),
                label: 'pass',
                count: passed,
              ),
              _SummaryBadge(
                color: const Color(0xFFF44336),
                label: 'fail',
                count: failed,
              ),
              _SummaryBadge(
                color: const Color(0xFFFF9800),
                label: 'skip',
                count: skipped,
              ),
            ],
          ),
        ),
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Per-check row widget
// ---------------------------------------------------------------------------

class _CheckRow extends StatelessWidget {
  const _CheckRow({required this.check});

  final Check check;

  @override
  Widget build(BuildContext context) {
    final (Color dotColor, String symbol) = switch (check.status) {
      CheckStatus.pending => (Colors.grey, '⬤'),
      CheckStatus.pass    => (const Color(0xFF4CAF50), '⬤'),
      CheckStatus.fail    => (const Color(0xFFF44336), '⬤'),
      CheckStatus.skip    => (const Color(0xFFFF9800), '⬤'),
    };

    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 10),
      child: Row(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(
            symbol,
            style: TextStyle(color: dotColor, fontSize: 18),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(
                  check.name,
                  style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                        fontWeight: FontWeight.bold,
                      ),
                ),
                if (check.detail.isNotEmpty)
                  Text(
                    check.detail,
                    style: Theme.of(context).textTheme.bodySmall?.copyWith(
                          color: Colors.white54,
                        ),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Bottom summary badge
// ---------------------------------------------------------------------------

class _SummaryBadge extends StatelessWidget {
  const _SummaryBadge({
    required this.color,
    required this.label,
    required this.count,
  });

  final Color color;
  final String label;
  final int count;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Text('⬤ ', style: TextStyle(color: color, fontSize: 16)),
        Text(
          '$label  $count',
          style: Theme.of(context).textTheme.bodyMedium,
        ),
      ],
    );
  }
}
