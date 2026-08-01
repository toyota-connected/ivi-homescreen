// Integration test app for multi-touch delivery against the ivi-homescreen
// embedder, sized for a 10-finger touch controller.
//
// The contract under test is the embedder's touch path:
//
//   wl_touch (or libinput TOUCH_*) -> FlutterEngineSendPointerEvent
//     -> PointerDataPacket -> framework PointerEvents on a raw Listener
//
// No GestureDetector and no gesture arena — a full-screen Listener receives
// the raw PointerDown/Move/Up/Cancel stream so nothing recognizes, claims,
// or swallows events before the checks see them.
//
// Checks (each flips green the first time it is satisfied; violations are
// counted and never reset except by the Reset button):
//
//   C1 concurrency   Peak simultaneous down contacts >= EXPECT_FINGERS
//                    (default 10) with that many *distinct* device ids.
//   C2 legality      Per-device phase machine: down only while up, move/up/
//                    cancel only while down. A violation here means the
//                    embedder lost, duplicated, or mis-routed a transition
//                    (e.g. the historical cancel-only-device-0 bug leaves 9
//                    contacts stuck down; the next session's down for those
//                    devices is a down-while-down violation).
//   C3 churn         Total distinct device ids observed across the run
//                    >= EXPECT_CHURN_IDS (default 24). Exercises compositor
//                    touch-id growth beyond any fixed 10-slot array — the
//                    pre-fix embedder indexed surface_x[id] and wrote out of
//                    bounds for id >= 10.
//   C4 frame batch   Events that arrive in one PointerDataPacket from one
//                    hardware scan share one timestamp (the embedder stamps
//                    the batch once). During a synchronized N-finger drag the
//                    mean move-group size (grouped by identical timeStamp)
//                    should approach N. Threshold EXPECT_BATCH_MEAN (default
//                    6.0 for a 10-finger drag). An unbatched embedder stamps
//                    each contact separately -> mean ~1.
//   C5 cancel        After a compositor cancel (e.g. a system gesture takes
//                    the touch session), every down contact receives
//                    PointerCancel — no contact may still be down 500 ms
//                    after a cancel arrives for any device. Advisory unless
//                    a cancel is actually observed.
//
// Two lines are printed whenever all contacts lift: "MTT-CHECKS:" with the
// five verdicts, and a summary JSON line prefixed "MTT-SUMMARY:".
// "MULTI_TOUCH_TEST: PASS|FAIL" follows whenever the Self-check button is
// tapped or SELFCHECK_AFTER_S (dart-define) elapses.
//
// The summary is longer than one log record, but the logging library splits
// and rejoins it, so it arrives on one line and parses. CI can read the JSON,
// or scrape "MTT-CHECKS:" / "MULTI_TOUCH_TEST:" and skip parsing entirely. The
// verdict flags come first for readability, not to survive a cut.
//
// Drive it with tools/inject_ten_finger.py (uinput, no hardware needed) or a
// real 10-point panel.

import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';

const int kExpectFingers =
    int.fromEnvironment('EXPECT_FINGERS', defaultValue: 10);
const int kExpectChurnIds =
    int.fromEnvironment('EXPECT_CHURN_IDS', defaultValue: 24);
// Dart has no double.fromEnvironment; read it as a string and parse.
const String _kExpectBatchMeanRaw =
    String.fromEnvironment('EXPECT_BATCH_MEAN', defaultValue: '6.0');
final double kExpectBatchMean = double.tryParse(_kExpectBatchMeanRaw) ?? 6.0;
// 0 disables the self-check timer (manual runs).
const int kSelfCheckAfterS =
    int.fromEnvironment('SELFCHECK_AFTER_S', defaultValue: 0);

void main() {
  FlutterError.onError = (FlutterErrorDetails details) {
    FlutterError.dumpErrorToConsole(details);
  };
  runApp(const MultiTouchTestApp());
}

class MultiTouchTestApp extends StatelessWidget {
  const MultiTouchTestApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(useMaterial3: true),
      home: const TouchProbePage(),
    );
  }
}

/// Everything observed about one embedder device id.
class DeviceState {
  bool down = false;
  Offset last = Offset.zero;
  int downs = 0;
  int moves = 0;
  int ups = 0;
  int cancels = 0;
}

class TouchProbePage extends StatefulWidget {
  const TouchProbePage({super.key});

  @override
  State<TouchProbePage> createState() => _TouchProbePageState();
}

class _TouchProbePageState extends State<TouchProbePage> {
  final Map<int, DeviceState> _devices = <int, DeviceState>{};
  final Set<int> _everSeenIds = <int>{};

  int _peakSimultaneous = 0;
  int _peakDistinctAtPeak = 0;
  int _legalityViolations = 0;
  final List<String> _violationLog = <String>[];

  // C4: move events grouped by identical timeStamp. A group is closed when a
  // move with a different timestamp arrives. Only groups observed while >= 2
  // contacts are down count toward the mean (single-finger motion trivially
  // batches to 1 and would dilute the metric).
  Duration? _openGroupTs;
  int _openGroupSize = 0;
  int _groupCount = 0;
  int _groupEventTotal = 0;
  int _largestGroup = 0;

  // C5: pending cancel deadline — set when any cancel arrives while other
  // contacts are down; if contacts remain down past it, that's a violation.
  Timer? _cancelDeadline;
  bool _cancelObserved = false;
  int _cancelViolations = 0;

  Timer? _selfCheckTimer;

  @override
  void initState() {
    super.initState();
    if (kSelfCheckAfterS > 0) {
      _selfCheckTimer =
          Timer(Duration(seconds: kSelfCheckAfterS), _printVerdict);
    }
  }

  @override
  void dispose() {
    _selfCheckTimer?.cancel();
    _cancelDeadline?.cancel();
    super.dispose();
  }

  DeviceState _dev(int id) => _devices.putIfAbsent(id, DeviceState.new);

  int get _downCount => _devices.values.where((d) => d.down).length;

  void _violation(String what) {
    _legalityViolations++;
    if (_violationLog.length < 50) {
      _violationLog.add(what);
    }
    debugPrint('MTT-VIOLATION: $what');
  }

  void _closeGroup() {
    if (_openGroupTs == null) return;
    _groupCount++;
    _groupEventTotal += _openGroupSize;
    if (_openGroupSize > _largestGroup) _largestGroup = _openGroupSize;
    _openGroupTs = null;
    _openGroupSize = 0;
  }

  void _trackGroup(Duration ts) {
    if (_downCount < 2) {
      _closeGroup();
      return;
    }
    if (_openGroupTs == ts) {
      _openGroupSize++;
    } else {
      _closeGroup();
      _openGroupTs = ts;
      _openGroupSize = 1;
    }
  }

  double get _meanBatch =>
      _groupCount == 0 ? 0 : _groupEventTotal / _groupCount;

  void _onDown(PointerDownEvent e) {
    final d = _dev(e.device);
    if (d.down) {
      _violation('down-while-down device=${e.device}');
    }
    d.down = true;
    d.downs++;
    d.last = e.position;
    _everSeenIds.add(e.device);
    _closeGroup(); // a down interleaved in the stream ends any move group

    final down = _downCount;
    if (down > _peakSimultaneous) {
      _peakSimultaneous = down;
      _peakDistinctAtPeak = _devices.entries
          .where((en) => en.value.down)
          .map((en) => en.key)
          .toSet()
          .length;
    }
    setState(() {});
  }

  void _onMove(PointerMoveEvent e) {
    final d = _dev(e.device);
    if (!d.down) {
      _violation('move-while-up device=${e.device}');
    }
    d.moves++;
    d.last = e.position;
    _trackGroup(e.timeStamp);
    setState(() {});
  }

  void _onUp(PointerUpEvent e) {
    final d = _dev(e.device);
    if (!d.down) {
      _violation('up-while-up device=${e.device}');
    }
    d.down = false;
    d.ups++;
    _closeGroup();
    setState(() {});
    if (_downCount == 0) _printSummary();
  }

  void _onCancel(PointerCancelEvent e) {
    final d = _dev(e.device);
    if (!d.down) {
      _violation('cancel-while-up device=${e.device}');
    }
    d.down = false;
    d.cancels++;
    _cancelObserved = true;
    _closeGroup();

    // C5: every contact that was down when the compositor cancelled the
    // session must also be cancelled. Give the rest of the batch 500 ms.
    _cancelDeadline?.cancel();
    _cancelDeadline = Timer(const Duration(milliseconds: 500), () {
      final stuck = _devices.entries
          .where((en) => en.value.down)
          .map((en) => en.key)
          .toList();
      if (stuck.isNotEmpty) {
        _cancelViolations += stuck.length;
        debugPrint('MTT-VIOLATION: contacts still down 500ms after cancel: '
            '$stuck');
        setState(() {});
      }
    });
    setState(() {});
    if (_downCount == 0) _printSummary();
  }

  // ---- verdict -----------------------------------------------------------

  bool get _c1 =>
      _peakSimultaneous >= kExpectFingers &&
      _peakDistinctAtPeak >= kExpectFingers;
  bool get _c2 => _legalityViolations == 0;
  bool get _c3 => _everSeenIds.length >= kExpectChurnIds;
  bool get _c4 => _groupCount > 0 && _meanBatch >= kExpectBatchMean;
  bool get _c5 => _cancelViolations == 0; // advisory if no cancel observed

  // Verdict flags first, counters after. The embedder caps a log record at
  // IHS_LOG_TEXT_CAPACITY and the encoded summary is longer than that, so
  // whatever sits at the end of this map is what gets clipped on the way out.
  // The flags are the part a reader cannot reconstruct; a missing counter is
  // merely inconvenient.
  Map<String, dynamic> _summary() => <String, dynamic>{
        'c1_concurrency': _c1,
        'c2_legality': _c2,
        'c3_churn': _c3,
        'c4_frame_batch': _c4,
        'c5_cancel': _c5,
        'peak_simultaneous': _peakSimultaneous,
        'peak_distinct_ids': _peakDistinctAtPeak,
        'distinct_ids_total': _everSeenIds.length,
        'legality_violations': _legalityViolations,
        'move_groups': _groupCount,
        'mean_batch': double.parse(_meanBatch.toStringAsFixed(2)),
        'largest_batch': _largestGroup,
        'cancel_observed': _cancelObserved,
        'cancel_violations': _cancelViolations,
      };

  // The five checks on one short line, well under any record cap, so a scrape
  // never has to parse a JSON line that may have been clipped.
  String _checksLine() => 'MTT-CHECKS: '
      'c1=${_c1 ? 'pass' : 'FAIL'} '
      'c2=${_c2 ? 'pass' : 'FAIL'} '
      'c3=${_c3 ? 'pass' : 'FAIL'} '
      'c4=${_c4 ? 'pass' : 'FAIL'} '
      'c5=${_c5 ? 'pass' : 'FAIL'}';

  void _printSummary() {
    debugPrint(_checksLine());
    debugPrint('MTT-SUMMARY: ${jsonEncode(_summary())}');
  }

  void _printVerdict() {
    _printSummary();
    final pass = _c1 && _c2 && _c3 && _c4 && _c5;
    debugPrint('MULTI_TOUCH_TEST: ${pass ? 'PASS' : 'FAIL'}');
  }

  void _reset() {
    setState(() {
      _devices.clear();
      _everSeenIds.clear();
      _peakSimultaneous = 0;
      _peakDistinctAtPeak = 0;
      _legalityViolations = 0;
      _violationLog.clear();
      _openGroupTs = null;
      _openGroupSize = 0;
      _groupCount = 0;
      _groupEventTotal = 0;
      _largestGroup = 0;
      _cancelObserved = false;
      _cancelViolations = 0;
    });
  }

  // ---- UI ----------------------------------------------------------------

  Widget _check(String label, bool ok, String detail, {bool advisory = false}) {
    final color = ok
        ? Colors.greenAccent
        : (advisory ? Colors.amberAccent : Colors.redAccent);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Row(children: [
        Icon(ok ? Icons.check_circle : Icons.radio_button_unchecked,
            color: color, size: 18),
        const SizedBox(width: 8),
        Expanded(
            child: Text('$label — $detail',
                style: TextStyle(color: color, fontSize: 13))),
      ]),
    );
  }

  @override
  Widget build(BuildContext context) {
    final active = _devices.entries.where((e) => e.value.down).toList()
      ..sort((a, b) => a.key.compareTo(b.key));

    return Scaffold(
      body: Listener(
        behavior: HitTestBehavior.opaque,
        onPointerDown: _onDown,
        onPointerMove: _onMove,
        onPointerUp: _onUp,
        onPointerCancel: _onCancel,
        child: Stack(children: [
          // Contact dots
          for (final e in active)
            Positioned(
              left: e.value.last.dx - 28,
              top: e.value.last.dy - 28,
              child: IgnorePointer(
                child: Container(
                  width: 56,
                  height: 56,
                  alignment: Alignment.center,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    color: Colors.primaries[e.key % Colors.primaries.length]
                        .withOpacity(0.6),
                  ),
                  child: Text('${e.key}',
                      style: const TextStyle(
                          fontWeight: FontWeight.bold, fontSize: 16)),
                ),
              ),
            ),
          // Scoreboard
          Positioned(
            left: 12,
            top: 12,
            child: IgnorePointer(
              child: Container(
                width: 460,
                padding: const EdgeInsets.all(12),
                decoration: BoxDecoration(
                  color: Colors.black.withOpacity(0.65),
                  borderRadius: BorderRadius.circular(8),
                ),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                        'down: $_downCount   peak: $_peakSimultaneous '
                        '(distinct ${_peakDistinctAtPeak})   '
                        'ids ever: ${_everSeenIds.length}',
                        style: const TextStyle(
                            fontSize: 14, fontWeight: FontWeight.bold)),
                    const Divider(height: 12),
                    _check(
                        'C1 concurrency',
                        _c1,
                        'peak $_peakSimultaneous/$kExpectFingers, '
                            'distinct $_peakDistinctAtPeak'),
                    _check(
                        'C2 legality', _c2, '$_legalityViolations violations'),
                    _check('C3 churn', _c3,
                        '${_everSeenIds.length}/$kExpectChurnIds ids'),
                    _check(
                        'C4 frame batch',
                        _c4,
                        'mean ${_meanBatch.toStringAsFixed(2)} '
                            '(>= $kExpectBatchMean), max $_largestGroup, '
                            '$_groupCount groups'),
                    _check(
                        'C5 cancel',
                        _c5,
                        _cancelObserved
                            ? '$_cancelViolations stuck after cancel'
                            : 'no cancel observed (advisory)',
                        advisory: !_cancelObserved),
                    if (_violationLog.isNotEmpty) ...[
                      const Divider(height: 12),
                      Text(_violationLog.take(4).join('\n'),
                          style: const TextStyle(
                              color: Colors.redAccent, fontSize: 11)),
                    ],
                  ],
                ),
              ),
            ),
          ),
          // Buttons (small corner targets; the injector never lands here)
          Positioned(
            right: 12,
            top: 12,
            child: Column(children: [
              FilledButton.tonal(
                  onPressed: _printVerdict, child: const Text('Self-check')),
              const SizedBox(height: 8),
              FilledButton.tonal(onPressed: _reset, child: const Text('Reset')),
            ]),
          ),
        ]),
      ),
    );
  }
}
