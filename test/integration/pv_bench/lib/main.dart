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
//   PV_BENCH_SCALE      wrap the view in Transform.scale(PV_BENCH_SCALE)
//                       about its center (default 1, no transform)
//   PV_BENCH_SCALE_ANIM set to 1 to wrap it in a ScaleTransition between 0.4
//                       and 1.0 instead, two seconds each way
//   PV_BENCH_CLIP       set to 1 to wrap it in a ClipRect that keeps only the
//                       middle of the screen, half its width and height
//   PV_BENCH_OPACITY    wrap the view in Opacity(PV_BENCH_OPACITY)
//                       (default 1, no opacity)
//   PV_BENCH_FADE       set to 1 to wrap it in a FadeTransition instead: up
//                       from 0.2 to 1.0 over two seconds, a second at 1.0,
//                       and back down over two
//   PV_BENCH_RADIUS     wrap the view in a ClipRRect with corners of this
//                       radius, in logical pixels (default 0, no clip)
//   PV_BENCH_ROTATE     wrap the view in Transform.rotate by this many
//                       degrees about its center (default 0)
//   PV_BENCH_ROTATE_ANIM set to 1 to wrap it in a RotationTransition instead:
//                       one turn every eight seconds
//   PV_BENCH_PERSPECTIVE tilt the view this many degrees about its vertical
//                       axis, in perspective (default 0)
//   PV_BENCH_INNER_RADIUS wrap the view itself in a ClipRRect with corners of
//                       this radius, inside every transform, so the clip
//                       turns, tilts and scales with it (default 0, no clip)
//   PV_BENCH_VIEWS      how many platform views to show (1 to 8, default 1).
//                       Past one, each is two thirds of the screen, and each
//                       sits a twelfth of the screen right of and below the
//                       last, with a spinner of its own over it

import 'dart:async';
import 'dart:ffi' as ffi;
import 'dart:io';
import 'dart:math' as math;

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
          'platform_view=${widget.showPlatformView} '
          'views=${widget.showPlatformView ? _pvViews : 0}')
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
          if (widget.showPlatformView && _pvViews > 1)
            Positioned.fill(child: _ViewStack(spin: _spin))
          else if (widget.showPlatformView)
            Positioned.fill(child: _wrapped(const _BenchPlatformView()))
          else
            const Center(
              child: Text('no platform view',
                  style: TextStyle(color: Colors.white54)),
            ),
          // Flutter content over the view, so the engine has something to
          // raster every frame. A static overlay would let the engine idle and
          // the comparison would be between two different workloads. With
          // several views each has its own (_ViewStack).
          if (!widget.showPlatformView || _pvViews == 1)
            Center(child: _Spinner(turns: _spin)),
        ],
      ),
    );
  }
}

// Every view gets the same mutations, in the same order.
Widget _wrapped(Widget view) => _faded(
    _rounded(_clipped(_rotated(_tilted(_scaled(_innerRounded(view)))))));

class _Spinner extends StatelessWidget {
  const _Spinner({required this.turns});

  final Animation<double> turns;

  @override
  Widget build(BuildContext context) {
    return RotationTransition(
      turns: turns,
      child: Container(
        width: 160,
        height: 160,
        decoration: BoxDecoration(
          color: Colors.amber.withValues(alpha: 0.85),
          borderRadius: BorderRadius.circular(24),
        ),
      ),
    );
  }
}

// Several views cost the shell more than one in two ways: a buffer and a
// composite per view, and a backing store per overlay. Flutter makes an
// overlay for the content above a view only when that content meets a view
// above it, and otherwise draws it in a later overlay or the root. So the
// views overlap, and each spinner but the last sits on the left edge of the
// next view, half under it, which gives each view an overlay of its own.
final int _pvViews =
    (int.tryParse(Platform.environment['PV_BENCH_VIEWS'] ?? '') ?? 1)
        .clamp(1, 8);

class _ViewStack extends StatelessWidget {
  const _ViewStack({required this.spin});

  final Animation<double> spin;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        final double w = constraints.maxWidth;
        final double h = constraints.maxHeight;
        final double stepX = w / 12;
        final double stepY = h / 12;
        final double viewW = w * 2 / 3;
        final double viewH = h * 2 / 3;
        const double half = 80; // half the spinner
        final List<Widget> children = <Widget>[];
        for (int i = 0; i < _pvViews; ++i) {
          final double left = stepX * i;
          final double top = stepY * i;
          final bool last = i == _pvViews - 1;
          final double cx = last ? left + viewW / 2 : left + stepX;
          final double cy = top + viewH / 2;
          children
            ..add(Positioned(
              left: left,
              top: top,
              width: viewW,
              height: viewH,
              child: _wrapped(const _BenchPlatformView()),
            ))
            ..add(Positioned(
              left: cx - half,
              top: cy - half,
              child: _Spinner(turns: spin),
            ));
        }
        return Stack(children: children);
      },
    );
  }
}

// A scale on the view puts a transform mutation on its layer, so a run can check
// that each backend draws the view at its scaled size and place (#865).
final double _pvScale =
    double.tryParse(Platform.environment['PV_BENCH_SCALE'] ?? '') ?? 1.0;
final bool _pvScaleAnim = Platform.environment['PV_BENCH_SCALE_ANIM'] == '1';

// A clip on the view puts a clip mutation on its layer, so a run can check that
// each backend crops the view to it (#863).
final bool _pvClip = Platform.environment['PV_BENCH_CLIP'] == '1';

Widget _clipped(Widget child) {
  if (!_pvClip) {
    return child;
  }
  return ClipRect(clipper: _MiddleClipper(), child: child);
}

// An opacity on the view puts an opacity mutation on its layer, so a run can
// check that each backend draws the view at it (#864).
final double _pvOpacity =
    double.tryParse(Platform.environment['PV_BENCH_OPACITY'] ?? '') ?? 1.0;
final bool _pvFade = Platform.environment['PV_BENCH_FADE'] == '1';

Widget _faded(Widget child) {
  if (_pvFade) {
    return _PulsingFade(child: child);
  }
  if (_pvOpacity == 1.0) {
    return child;
  }
  return Opacity(opacity: _pvOpacity.clamp(0.0, 1.0), child: child);
}

class _PulsingFade extends StatefulWidget {
  const _PulsingFade({required this.child});

  final Widget child;

  @override
  State<_PulsingFade> createState() => _PulsingFadeState();
}

class _PulsingFadeState extends State<_PulsingFade>
    with SingleTickerProviderStateMixin {
  late final AnimationController _pulse = AnimationController(
    vsync: this,
    duration: const Duration(seconds: 5),
  )..repeat();

  // The second at 1.0 is there because Flutter drops the opacity at 1.0: the
  // view is then opaque for long enough to check that it shows that way.
  static final Animatable<double> _opacity = TweenSequence<double>([
    TweenSequenceItem(tween: Tween<double>(begin: 0.2, end: 1.0), weight: 2),
    TweenSequenceItem(tween: ConstantTween<double>(1.0), weight: 1),
    TweenSequenceItem(tween: Tween<double>(begin: 1.0, end: 0.2), weight: 2),
  ]);

  @override
  void dispose() {
    _pulse.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return FadeTransition(
      opacity: _opacity.animate(_pulse),
      child: widget.child,
    );
  }
}

// A rounded clip on the view puts a rounded-rect clip mutation on its layer,
// so a run can check that each backend cuts the view's corners (#867).
final double _pvRadius =
    double.tryParse(Platform.environment['PV_BENCH_RADIUS'] ?? '') ?? 0.0;

Widget _rounded(Widget child) {
  if (_pvRadius <= 0.0) {
    return child;
  }
  return ClipRRect(
    borderRadius: BorderRadius.circular(_pvRadius),
    child: child,
  );
}

// A rounded clip inside the view's transforms is not a rounded rect on screen
// once they turn or tilt it, and with PV_BENCH_RADIUS the view has two, so a
// run can check that each backend draws its clips as they are (#885).
final double _pvInnerRadius =
    double.tryParse(Platform.environment['PV_BENCH_INNER_RADIUS'] ?? '') ??
        0.0;

Widget _innerRounded(Widget child) {
  if (_pvInnerRadius <= 0.0) {
    return child;
  }
  return ClipRRect(
    borderRadius: BorderRadius.circular(_pvInnerRadius),
    child: child,
  );
}

// A rotation of the view puts a transform the shell cannot show as a rect on
// its layer, so a run can check that each backend draws it turned (#866).
final double _pvRotate =
    double.tryParse(Platform.environment['PV_BENCH_ROTATE'] ?? '') ?? 0.0;
final bool _pvRotateAnim = Platform.environment['PV_BENCH_ROTATE_ANIM'] == '1';

Widget _rotated(Widget child) {
  if (_pvRotateAnim) {
    return _Spinning(child: child);
  }
  if (_pvRotate == 0.0) {
    return child;
  }
  return Transform.rotate(angle: _pvRotate * math.pi / 180.0, child: child);
}

// A perspective on the view puts a transform with a perspective row on its
// layer, so a run can check that each backend draws it in perspective (#868).
final double _pvPerspective =
    double.tryParse(Platform.environment['PV_BENCH_PERSPECTIVE'] ?? '') ?? 0.0;

Widget _tilted(Widget child) {
  if (_pvPerspective == 0.0) {
    return child;
  }
  return Transform(
    alignment: Alignment.center,
    transform: Matrix4.identity()
      ..setEntry(3, 2, 0.001)
      ..rotateY(_pvPerspective * math.pi / 180.0),
    child: child,
  );
}

class _Spinning extends StatefulWidget {
  const _Spinning({required this.child});

  final Widget child;

  @override
  State<_Spinning> createState() => _SpinningState();
}

class _SpinningState extends State<_Spinning>
    with SingleTickerProviderStateMixin {
  late final AnimationController _turn = AnimationController(
    vsync: this,
    duration: const Duration(seconds: 8),
  )..repeat();

  @override
  void dispose() {
    _turn.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return RotationTransition(turns: _turn, child: widget.child);
  }
}

class _MiddleClipper extends CustomClipper<Rect> {
  @override
  Rect getClip(Size size) => Rect.fromLTWH(
      size.width / 4, size.height / 4, size.width / 2, size.height / 2);

  @override
  bool shouldReclip(_MiddleClipper oldClipper) => false;
}

Widget _scaled(Widget child) {
  if (_pvScaleAnim) {
    return _PulsingScale(child: child);
  }
  if (_pvScale == 1.0) {
    return child;
  }
  return Transform.scale(scale: _pvScale, child: child);
}

class _PulsingScale extends StatefulWidget {
  const _PulsingScale({required this.child});

  final Widget child;

  @override
  State<_PulsingScale> createState() => _PulsingScaleState();
}

class _PulsingScaleState extends State<_PulsingScale>
    with SingleTickerProviderStateMixin {
  late final AnimationController _pulse = AnimationController(
    vsync: this,
    duration: const Duration(seconds: 2),
  )..repeat(reverse: true);

  @override
  void dispose() {
    _pulse.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return ScaleTransition(
      scale: Tween<double>(begin: 0.4, end: 1.0).animate(_pulse),
      child: widget.child,
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
