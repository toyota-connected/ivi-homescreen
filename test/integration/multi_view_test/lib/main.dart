// Multi-monitor single-engine integration test app for ivi-homescreen.
//
// One Flutter engine renders to N views (one per output). The proof that a
// SINGLE engine is driving every monitor is the shared `tick` counter: it lives
// in one isolate and every view shows the identical value in lockstep. Each
// view also shows its own "VIEW n" label and a distinct background colour, so
// you can tell the outputs apart and confirm each is a separate view.
//
// This uses Flutter's multi-view API (runWidget + View/ViewCollection). It must
// be built against an engine that supports multi-view; verify at bundle-build
// time. Extra views appear via FlutterEngineAddView on the embedder side.

import 'dart:async';
import 'dart:ui' show FlutterView;

import 'package:flutter/widgets.dart';

void main() {
  runWidget(const MultiViewTestApp());
}

/// Renders one widget tree per live FlutterView and rebuilds when the set of
/// views changes (embedder AddView/RemoveView). Owns the shared tick that
/// proves a single engine drives all views.
class MultiViewTestApp extends StatefulWidget {
  const MultiViewTestApp({super.key});

  @override
  State<MultiViewTestApp> createState() => _MultiViewTestAppState();
}

class _MultiViewTestAppState extends State<MultiViewTestApp>
    with WidgetsBindingObserver {
  final ValueNotifier<int> _tick = ValueNotifier<int>(0);
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    // One timer in one isolate. Every view reads this same notifier, so a
    // single engine shows the same value on every monitor in lockstep.
    _timer = Timer.periodic(const Duration(milliseconds: 100), (_) {
      _tick.value = _tick.value + 1;
    });
  }

  // Fires when a view is added, removed, or resized — rebuild the collection.
  @override
  void didChangeMetrics() => setState(() {});

  @override
  void dispose() {
    _timer?.cancel();
    WidgetsBinding.instance.removeObserver(this);
    _tick.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final List<FlutterView> views =
        WidgetsBinding.instance.platformDispatcher.views.toList();
    return ViewCollection(
      views: <Widget>[
        for (int i = 0; i < views.length; i++)
          View(
            view: views[i],
            child: _ViewContent(
              index: i,
              viewId: views[i].viewId,
              tick: _tick,
            ),
          ),
      ],
    );
  }
}

/// A self-contained per-view tree (no MaterialApp — that assumes one view).
class _ViewContent extends StatelessWidget {
  const _ViewContent({
    required this.index,
    required this.viewId,
    required this.tick,
  });

  final int index;
  final int viewId;
  final ValueNotifier<int> tick;

  static const List<Color> _colors = <Color>[
    Color(0xFF1565C0), // blue
    Color(0xFFC62828), // red
    Color(0xFF2E7D32), // green
    Color(0xFF6A1B9A), // purple
  ];

  @override
  Widget build(BuildContext context) {
    return Directionality(
      textDirection: TextDirection.ltr,
      child: DefaultTextStyle(
        style: const TextStyle(color: Color(0xFFFFFFFF), fontSize: 32),
        child: ColoredBox(
          color: _colors[index % _colors.length],
          child: Center(
            child: ValueListenableBuilder<int>(
              valueListenable: tick,
              builder: (BuildContext context, int t, Widget? _) {
                return Column(
                  mainAxisSize: MainAxisSize.min,
                  children: <Widget>[
                    Text(
                      'VIEW $index',
                      style: const TextStyle(
                        fontSize: 120,
                        fontWeight: FontWeight.bold,
                      ),
                    ),
                    Text('viewId $viewId'),
                    const SizedBox(height: 24),
                    // Shared across all views -> identical on every monitor when
                    // one engine drives them. A mismatch means separate engines.
                    Text(
                      'tick $t',
                      style: const TextStyle(fontSize: 72),
                    ),
                  ],
                );
              },
            ),
          ),
        ),
      ),
    );
  }
}
