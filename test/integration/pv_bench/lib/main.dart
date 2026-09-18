// Copyright 2026 Toyota Connected North America
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Puts one platform view on screen with Flutter content over it, and reports
// frame timings the same way scroll_bench does.
//
// The platform view comes from libpv_bench_producer.so, loaded over FFI here
// rather than through the shell's plugin loader -- so no shell rebuild, and the
// producer is exactly as out-of-tree as a real one.
//
//   PV_BENCH_PRODUCER   path to libpv_bench_producer.so (default: alongside
//                       the shell's libs, ./lib/libpv_bench_producer.so)
//   PV_BENCH_NO_PV      set to run the Flutter content with no platform view,
//                       for the layers-off baseline
//   TIMINGS_FILE        where to write per-frame timings (default: stdout only)
//   BENCH_SECONDS       run length before reporting and exiting (default 20)

import 'dart:async';
import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter/services.dart';

const String _viewType = 'pv_bench';

typedef _VoidNative = ffi.Void Function();
typedef _VoidDart = void Function();

/// Loads the producer and registers its factory.
///
/// Registration is platform-thread only. On this embedder the Dart entry point
/// runs there, which is the same assumption every other ihs_pv producer makes.
bool registerProducer() {
  final String path =
      Platform.environment['PV_BENCH_PRODUCER'] ?? 'lib/libpv_bench_producer.so';
  try {
    final ffi.DynamicLibrary lib = ffi.DynamicLibrary.open(path);
    lib.lookupFunction<_VoidNative, _VoidDart>('pv_bench_register')();
    print('[pv_bench] registered producer from $path');
    return true;
  } on Object catch (e) {
    // Worth being loud about: with no factory the shell creates no view, the
    // app still runs, and the numbers would silently describe an empty scene.
    print('[pv_bench] FAILED to load $path: $e');
    return false;
  }
}

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  final bool wantPv = Platform.environment['PV_BENCH_NO_PV'] == null;
  final bool haveProducer = wantPv && registerProducer();
  runApp(PvBenchApp(showPlatformView: haveProducer));
}

class PvBenchApp extends StatelessWidget {
  const PvBenchApp({super.key, required this.showPlatformView});

  final bool showPlatformView;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      home: PvBenchPage(showPlatformView: showPlatformView),
    );
  }
}

class PvBenchPage extends StatefulWidget {
  const PvBenchPage({super.key, required this.showPlatformView});

  final bool showPlatformView;

  @override
  State<PvBenchPage> createState() => _PvBenchPageState();
}

class _PvBenchPageState extends State<PvBenchPage>
    with SingleTickerProviderStateMixin {
  late final AnimationController _spin = AnimationController(
    vsync: this,
    duration: const Duration(seconds: 3),
  )..repeat();

  final List<FrameTiming> _timings = <FrameTiming>[];
  Timer? _stopTimer;

  @override
  void initState() {
    super.initState();
    SchedulerBinding.instance.addTimingsCallback(_onTimings);
    final int seconds =
        int.tryParse(Platform.environment['BENCH_SECONDS'] ?? '') ?? 20;
    _stopTimer = Timer(Duration(seconds: seconds), () => unawaited(_report()));
  }

  void _onTimings(List<FrameTiming> timings) => _timings.addAll(timings);

  // Async so the exit can wait. The report goes out through the embedder's log
  // ring, and exiting the instant after printing tears the process down with
  // those records still in it -- a benchmark that runs for its full duration,
  // exits 0, and says nothing.
  Future<void> _report() async {
    // Skip the first second: startup, shader warm-up and the producer's first
    // pass round its ring are not what this is measuring.
    final List<FrameTiming> t = _timings.length > 60
        ? _timings.sublist(60)
        : _timings;
    if (t.isEmpty) {
      print('[pv_bench] no frames recorded');
      await Future<void>.delayed(const Duration(milliseconds: 500));
      exit(1);
    }
    final List<int> build = <int>[];
    final List<int> raster = <int>[];
    final List<int> total = <int>[];
    for (final FrameTiming f in t) {
      build.add(f.buildDuration.inMicroseconds);
      raster.add(f.rasterDuration.inMicroseconds);
      total.add(f.totalSpan.inMicroseconds);
    }
    build.sort();
    raster.sort();
    total.sort();
    int pct(List<int> v, double p) => v[((v.length - 1) * p).round()];
    final double meanTotal =
        total.reduce((a, b) => a + b) / total.length / 1000.0;

    final StringBuffer out = StringBuffer()
      ..writeln('[pv_bench] frames=${t.length} '
          'platform_view=${widget.showPlatformView}')
      ..writeln('[pv_bench] fps=${(1000.0 / meanTotal).toStringAsFixed(2)}')
      ..writeln('[pv_bench] build  p50=${pct(build, 0.5) / 1000.0} '
          'p90=${pct(build, 0.9) / 1000.0} p99=${pct(build, 0.99) / 1000.0} ms')
      ..writeln('[pv_bench] raster p50=${pct(raster, 0.5) / 1000.0} '
          'p90=${pct(raster, 0.9) / 1000.0} p99=${pct(raster, 0.99) / 1000.0} ms')
      ..writeln('[pv_bench] total  p50=${pct(total, 0.5) / 1000.0} '
          'p90=${pct(total, 0.9) / 1000.0} p99=${pct(total, 0.99) / 1000.0} ms');
    // print, not debugPrint: debugPrint throttles to roughly a kilobyte a
    // second and queues the rest, and the exit below throws that queue away --
    // which is a report that runs, says nothing, and exits 0.
    print(out.toString().trimRight());

    final String? file = Platform.environment['TIMINGS_FILE'];
    if (file != null) {
      final StringBuffer csv = StringBuffer('build_us,raster_us,total_us\n');
      for (final FrameTiming f in t) {
        csv.writeln('${f.buildDuration.inMicroseconds},'
            '${f.rasterDuration.inMicroseconds},'
            '${f.totalSpan.inMicroseconds}');
      }
      File(file).writeAsStringSync(csv.toString());
      print('[pv_bench] wrote $file');
    }
    // The summary also goes to a file, so a run whose stdout is lost to the log
    // ring is still readable afterwards.
    final String? summary = Platform.environment['SUMMARY_FILE'];
    if (summary != null) {
      File(summary).writeAsStringSync(out.toString());
    }
    await Future<void>.delayed(const Duration(milliseconds: 500));
    exit(0);
  }

  @override
  void dispose() {
    _stopTimer?.cancel();
    SchedulerBinding.instance.removeTimingsCallback(_onTimings);
    _spin.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: Colors.black,
      body: Stack(
        fit: StackFit.expand,
        children: <Widget>[
          if (widget.showPlatformView)
            const Positioned.fill(child: _BenchPlatformView())
          else
            const Center(
              child: Text('no platform view',
                  style: TextStyle(color: Colors.white54)),
            ),
          // Flutter content over the view, so the engine has something to
          // raster every frame. A static overlay would let the engine idle and
          // the comparison would be between two different workloads.
          Center(
            child: RotationTransition(
              turns: _spin,
              child: Container(
                width: 160,
                height: 160,
                decoration: BoxDecoration(
                  color: Colors.amber.withValues(alpha: 0.85),
                  borderRadius: BorderRadius.circular(24),
                ),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _BenchPlatformView extends StatelessWidget {
  const _BenchPlatformView();

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        final double dpr = MediaQuery.of(context).devicePixelRatio;
        return PlatformViewLink(
          viewType: _viewType,
          surfaceFactory: (BuildContext context, PlatformViewController c) {
            return PlatformViewSurface(
              controller: c,
              hitTestBehavior: PlatformViewHitTestBehavior.transparent,
              gestureRecognizers: const <Factory<OneSequenceGestureRecognizer>>{},
            );
          },
          onCreatePlatformView: (PlatformViewCreationParams params) {
            final _BenchViewController controller = _BenchViewController(
              id: params.id,
              width: constraints.maxWidth * dpr,
              height: constraints.maxHeight * dpr,
            );
            unawaited(controller.create().then((_) {
              params.onPlatformViewCreated(params.id);
            }));
            return controller;
          },
        );
      },
    );
  }
}

// Extends rather than implements, so members added to the base later do not
// break the build.
class _BenchViewController extends PlatformViewController {
  _BenchViewController({
    required this.id,
    required this.width,
    required this.height,
  });

  final int id;
  final double width;
  final double height;

  bool _created = false;
  Future<void>? _creation;

  @override
  int get viewId => id;

  @override
  bool get awaitingCreation => !_created;

  @override
  Future<void> create({Size? size, Offset? position}) =>
      _creation ??= _createOnce();

  Future<void> _createOnce() async {
    await SystemChannels.platform_views.invokeMethod<void>(
      'create',
      <String, Object>{
        'id': id,
        'viewType': _viewType,
        'direction': 0,
        'width': width,
        'height': height,
      },
    );
    _created = true;
  }

  @override
  Future<void> dispatchPointerEvent(PointerEvent event) async {}

  @override
  Future<void> clearFocus() async {}

  @override
  Future<void> dispose() async {
    if (!_created) {
      return;
    }
    await SystemChannels.platform_views
        .invokeMethod<void>('dispose', <String, Object>{'id': id});
  }
}
