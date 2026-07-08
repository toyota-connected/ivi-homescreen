import 'dart:math';
import 'dart:ui' as ui;

import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'map_gesture_detector/map_gesture_detector.dart';

/// Live diagnostic surface for exercising the full pointer input surface of an
/// embedder rather than just single-touch: every [PointerDeviceKind],
/// simultaneous (3+) pointer tracking, stylus pressure / tilt / orientation,
/// hover with cursor-shape changes, engine pointer coalescing, and event
/// timestamps.
///
/// A left-button drag drives the real [MapGestureDetector] recognizers (now
/// device-agnostic). A right-button drag, optionally with Shift / Alt / Ctrl,
/// simulates a two-finger pinch / pitch / rotate with a single mouse.
class MouseTestFrame extends StatefulWidget {
  ///
  const MouseTestFrame({super.key, this.child});

  /// Optional underlay painted behind the diagnostics.
  final Widget? child;

  @override
  State<MouseTestFrame> createState() => _MouseTestFrameState();
}

class _MouseTestFrameState extends State<MouseTestFrame> {
  late final FocusNode _focusNode = FocusNode();

  final Map<int, _PointerSample> _pointers = <int, _PointerSample>{};

  Offset? _hoverPosition;
  PointerDeviceKind? _hoverKind;

  bool _shiftPressed = false;
  bool _altPressed = false;
  bool _ctrlPressed = false;

  double? _scaleStart;
  double? _angleStart;

  String _lastGesture = '(none)';

  // Timestamp / coalescing diagnostics. Framework move events are already
  // coalesced and resampled, so the only place the raw sample rate is visible
  // is the pointer-data packet the engine delivers each frame.
  Duration _lastMoveTimeStamp = Duration.zero;
  Duration _lastMoveDelta = Duration.zero;
  int _frameworkMoves = 0;
  int _rawMoveSamples = 0;
  ui.PointerDataPacketCallback? _previousPacketCallback;

  @override
  void initState() {
    super.initState();
    // Chain the raw packet callback so we can count the samples the engine
    // coalesced into each frame without disturbing normal input routing.
    _previousPacketCallback =
        ui.PlatformDispatcher.instance.onPointerDataPacket;
    ui.PlatformDispatcher.instance.onPointerDataPacket = _handlePacket;
  }

  @override
  void dispose() {
    ui.PlatformDispatcher.instance.onPointerDataPacket =
        _previousPacketCallback;
    _focusNode.dispose();
    super.dispose();
  }

  void _handlePacket(ui.PointerDataPacket packet) {
    for (final data in packet.data) {
      if (data.change == ui.PointerChange.move ||
          data.change == ui.PointerChange.hover) {
        _rawMoveSamples++;
      }
    }
    _previousPacketCallback?.call(packet);
  }

  void _onKeyEvent(KeyEvent event) {
    final keyboard = HardwareKeyboard.instance;
    if (_shiftPressed != keyboard.isShiftPressed ||
        _altPressed != keyboard.isAltPressed ||
        _ctrlPressed != keyboard.isControlPressed) {
      setState(() {
        _shiftPressed = keyboard.isShiftPressed;
        _altPressed = keyboard.isAltPressed;
        _ctrlPressed = keyboard.isControlPressed;
      });
    }
  }

  _PointerSample _sampleFrom(PointerEvent event) => _PointerSample(
        kind: event.kind,
        position: event.localPosition,
        pressure: event.pressure,
        tilt: event.tilt,
        orientation: event.orientation,
        buttons: event.buttons,
      );

  void _onPointerDown(PointerDownEvent event, BoxConstraints constraints) {
    final secondary = event.kind == PointerDeviceKind.mouse &&
        event.buttons & kSecondaryMouseButton != 0;
    setState(() {
      _pointers[event.pointer] = _sampleFrom(event);
      if (secondary) {
        final center = _center(constraints);
        _scaleStart = (event.localPosition - center).distance;
        _angleStart = _angle(center, event.localPosition);
      }
    });
  }

  void _onPointerMove(PointerMoveEvent event, BoxConstraints constraints) {
    setState(() {
      _pointers[event.pointer] = _sampleFrom(event);
      _lastMoveDelta = event.timeStamp - _lastMoveTimeStamp;
      _lastMoveTimeStamp = event.timeStamp;
      _frameworkMoves++;
      _simulateTwoFinger(event, constraints);
    });
  }

  // Reconstruct a two-finger gesture from a single right-button mouse drag so
  // pinch / pitch / rotate can be driven without a touchscreen.
  void _simulateTwoFinger(PointerMoveEvent event, BoxConstraints constraints) {
    final secondary = event.kind == PointerDeviceKind.mouse &&
        event.buttons & kSecondaryMouseButton != 0;
    if (!secondary) {
      return;
    }
    final center = _center(constraints);
    if (_shiftPressed) {
      final distance = (event.localPosition - center).distance;
      final start = _scaleStart ?? distance;
      final scale = (distance - start) / (start == 0 ? 1 : start);
      _scaleStart = distance;
      _lastGesture = 'mouse pinch  scale=${scale.toStringAsFixed(3)}';
    } else if (_altPressed) {
      final pitch = -event.localDelta.dy * _pitchPerPixel(constraints);
      _lastGesture = 'mouse pitch  d=${pitch.toStringAsFixed(2)}';
    } else if (_ctrlPressed) {
      final current = _angle(center, event.localPosition);
      final rotation = ((_angleStart ?? current) - current) * 180 / pi;
      _angleStart = current;
      _lastGesture = 'mouse rotate deg=${rotation.toStringAsFixed(1)}';
    } else {
      _lastGesture = 'mouse pan    ${event.localDelta}';
    }
  }

  void _onPointerUp(PointerUpEvent event) {
    setState(() {
      _pointers.remove(event.pointer);
      _scaleStart = null;
      _angleStart = null;
    });
  }

  void _onPointerCancel(PointerCancelEvent event) {
    setState(() => _pointers.remove(event.pointer));
  }

  void _onPointerSignal(PointerSignalEvent event) {
    if (event is PointerScrollEvent) {
      setState(() {
        _lastGesture = 'wheel dy=${event.scrollDelta.dy.toStringAsFixed(1)}';
      });
    }
  }

  void _onHover(PointerHoverEvent event) {
    setState(() {
      _hoverPosition = event.localPosition;
      _hoverKind = event.kind;
      _frameworkMoves++;
    });
  }

  void _onExit(PointerExitEvent event) {
    setState(() {
      _hoverPosition = null;
      _hoverKind = null;
    });
  }

  void _recognized(String label) {
    setState(() => _lastGesture = 'recognizer $label');
  }

  // Cursor shape reacts to state, exercising the embedder's cursor path.
  MouseCursor get _cursor {
    if (_pointers.values.any((s) => s.buttons != 0)) {
      return SystemMouseCursors.grabbing;
    }
    if (_hoverPosition != null) {
      return SystemMouseCursors.grab;
    }
    return MouseCursor.defer;
  }

  Offset _center(BoxConstraints constraints) =>
      Offset(constraints.maxWidth / 2, constraints.maxHeight / 2);

  double _angle(Offset center, Offset target) => (target - center).direction;

  double _pitchPerPixel(BoxConstraints constraints) =>
      90 / constraints.maxHeight;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(
      builder: (context, constraints) {
        return KeyboardListener(
          focusNode: _focusNode,
          autofocus: true,
          onKeyEvent: _onKeyEvent,
          child: MouseRegion(
            cursor: _cursor,
            onExit: _onExit,
            onHover: _onHover,
            child: Listener(
              behavior: HitTestBehavior.translucent,
              onPointerDown: (event) => _onPointerDown(event, constraints),
              onPointerMove: (event) => _onPointerMove(event, constraints),
              onPointerUp: _onPointerUp,
              onPointerCancel: _onPointerCancel,
              onPointerSignal: _onPointerSignal,
              child: MapGestureDetector(
                onTap: () => _recognized('tap'),
                onLongPress: () => _recognized('longPress'),
                onScroll: (delta) => _recognized('pan $delta'),
                onScrollEnd: (position, velocity) => _recognized('pan end'),
                onPinch: (center, scale) =>
                    _recognized('pinch ${scale.toStringAsFixed(3)}'),
                onRotate: (angle) =>
                    _recognized('rotate ${angle.toStringAsFixed(1)}'),
                onPitch: (delta) =>
                    _recognized('pitch ${delta.toStringAsFixed(1)}'),
                onThreeFingerSwipe: (delta) =>
                    _recognized('3-finger swipe $delta'),
                onThreeFingerSwipeEnd: (position, velocity) =>
                    _recognized('3-finger swipe end'),
                child: Stack(
                  fit: StackFit.expand,
                  children: <Widget>[
                    if (widget.child != null) widget.child!,
                    Positioned.fill(
                      child: CustomPaint(
                        painter: _PointerPainter(
                          pointers: _pointers,
                          hover: _hoverPosition,
                        ),
                      ),
                    ),
                    Positioned(
                      left: 12,
                      top: 12,
                      child: IgnorePointer(child: _buildHud()),
                    ),
                  ],
                ),
              ),
            ),
          ),
        );
      },
    );
  }

  Widget _buildHud() {
    final ratio =
        _frameworkMoves == 0 ? 0.0 : _rawMoveSamples / _frameworkMoves;
    final buffer = StringBuffer()
      ..writeln('active pointers: ${_pointers.length}')
      ..writeln('last gesture:    $_lastGesture');
    for (final entry in _pointers.entries) {
      final sample = entry.value;
      buffer.writeln(
        '  #${entry.key} ${sample.kind.name}  '
        'p=${sample.pressure.toStringAsFixed(2)} '
        'tilt=${sample.tilt.toStringAsFixed(2)} '
        'orient=${sample.orientation.toStringAsFixed(2)}',
      );
    }
    buffer
      ..writeln('hover:     ${_describeHover()}')
      ..writeln('modifiers: ${_describeModifiers()}')
      ..writeln(
        'move dt:   ${_lastMoveDelta.inMicroseconds / 1000}ms  '
        '(stamp ${_lastMoveTimeStamp.inMilliseconds}ms)',
      )
      ..write(
        'moves raw/fw: $_rawMoveSamples/$_frameworkMoves '
        '(${ratio.toStringAsFixed(2)}x coalesced)',
      );
    return DecoratedBox(
      decoration: const BoxDecoration(color: Color(0xCC000000)),
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Text(
          buffer.toString(),
          style: const TextStyle(
            color: Color(0xFFFFFFFF),
            fontFamily: 'monospace',
            fontSize: 12,
            height: 1.35,
          ),
        ),
      ),
    );
  }

  String _describeHover() {
    final position = _hoverPosition;
    if (position == null) {
      return 'none';
    }
    return '${_hoverKind?.name ?? '?'} '
        '(${position.dx.toStringAsFixed(0)}, '
        '${position.dy.toStringAsFixed(0)})';
  }

  String _describeModifiers() {
    final mods = <String>[
      if (_shiftPressed) 'shift',
      if (_altPressed) 'alt',
      if (_ctrlPressed) 'ctrl',
    ];
    return mods.isEmpty ? 'none' : mods.join('+');
  }
}

/// A snapshot of one active pointer's richness at the last event.
class _PointerSample {
  _PointerSample({
    required this.kind,
    required this.position,
    required this.pressure,
    required this.tilt,
    required this.orientation,
    required this.buttons,
  });

  final PointerDeviceKind kind;
  final Offset position;
  final double pressure;
  final double tilt;
  final double orientation;
  final int buttons;
}

class _PointerPainter extends CustomPainter {
  _PointerPainter({required this.pointers, required this.hover});

  final Map<int, _PointerSample> pointers;
  final Offset? hover;

  @override
  void paint(Canvas canvas, Size size) {
    final hoverPosition = hover;
    if (hoverPosition != null) {
      final ring = Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2
        ..color = const Color(0x88FFFFFF);
      canvas.drawCircle(hoverPosition, 16, ring);
    }

    final fill = Paint()..color = const Color(0xAAFF5722);
    for (final sample in pointers.values) {
      // Radius grows with reported pressure (visible for touch and stylus).
      final radius = 12 + sample.pressure * 24;
      canvas.drawCircle(sample.position, radius, fill);
    }

    // Connect the first two pointers to visualize a two-finger span.
    if (pointers.length >= 2) {
      final positions = pointers.values.toList();
      final line = Paint()
        ..color = const Color(0xAAFFFFFF)
        ..strokeWidth = 2;
      canvas.drawLine(positions[0].position, positions[1].position, line);
    }
  }

  @override
  bool shouldRepaint(_PointerPainter oldDelegate) => true;
}
