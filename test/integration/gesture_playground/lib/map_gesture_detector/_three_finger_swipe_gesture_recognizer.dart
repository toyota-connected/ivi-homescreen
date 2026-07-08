import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';

/// Reports the centroid delta of the three pointers on each move.
typedef GestureThreeFingerSwipeCallback = void Function(Offset delta);

/// Reports the final centroid position and its velocity when the swipe ends.
typedef GestureThreeFingerSwipeEndCallback = void Function(
  Offset position,
  VelocityEstimate? velocityEstimate,
);

/// Recognizes a three-finger swipe: it arms once exactly three pointers are
/// down, activates when their centroid moves past [slop], then reports the
/// centroid delta on every move and a velocity estimate when a pointer lifts.
///
/// Unlike the one/two-finger recognizers, this tracks three simultaneous
/// pointers. Any fourth pointer, or the first release, ends the gesture; the
/// interpretation only re-arms once every pointer has lifted.
class ThreeFingerSwipeGestureRecognizer extends GestureRecognizer {
  ///
  ThreeFingerSwipeGestureRecognizer({
    this.slop = 0,
    this.claimVictoryOnStart = false,
    super.supportedDevices,
  });

  @override
  String get debugDescription => 'ThreeFingerSwipe';

  ///Called with the centroid delta each time the three pointers move.
  GestureThreeFingerSwipeCallback? onSwipe;

  ///Called when the swipe ends (a pointer lifts) with the last centroid and a
  ///velocity estimate.
  GestureThreeFingerSwipeEndCallback? onSwipeEnd;

  ///Distance the centroid must travel from its starting position before the
  ///gesture activates and begins sending callbacks.
  final double slop;

  ///Whether the gesture claims exclusive ownership of its pointers once slop
  ///is exceeded and the gesture begins.
  final bool claimVictoryOnStart;

  ///Provides the current activation state of the gesture.
  ThreeFingerState swipeState = const ThreeFingerStateIdle();

  static const int _fingers = 3;

  final Map<int, _Tracker> _pointers = <int, _Tracker>{};
  bool _abandoned = false;

  Offset? _centroid() {
    if (_pointers.length < _fingers) {
      return null;
    }
    var dx = 0.0;
    var dy = 0.0;
    for (final tracker in _pointers.values) {
      dx += tracker.position.dx;
      dy += tracker.position.dy;
    }
    return Offset(dx / _pointers.length, dy / _pointers.length);
  }

  @override
  void addAllowedPointer(PointerDownEvent event) {
    super.addAllowedPointer(event);
    final entry = GestureBinding.instance.gestureArena.add(event.pointer, this);
    _pointers[event.pointer] = _Tracker(event, entry, _TrackerState.pending);
    GestureBinding.instance.pointerRouter
        .addRoute(event.pointer, _handleEvent, event.transform);

    if (!_abandoned &&
        _pointers.length == _fingers &&
        swipeState is ThreeFingerStateIdle) {
      swipeState = ThreeFingerStateWaitingForSlop(_centroid()!);
    } else if (_pointers.length > _fingers &&
        swipeState is! ThreeFingerStateIdle) {
      _end(_centroid());
    }
  }

  void _handleEvent(PointerEvent event) {
    final tracker = _pointers[event.pointer];
    if (tracker == null) {
      return;
    }
    tracker._lastEvent = event;
    if (event is PointerMoveEvent) {
      _onMove();
    } else if (event is PointerUpEvent || event is PointerCancelEvent) {
      _end(_centroid());
      tracker.lose();
      _untrack(tracker.pointer);
    }
  }

  void _onMove() {
    final centroid = _centroid();
    if (centroid == null) {
      return;
    }
    switch (swipeState) {
      case final ThreeFingerStateWaitingForSlop waiting:
        if ((centroid - waiting.startCentroid).distance >= slop) {
          swipeState = ThreeFingerStateActive(
            centroid,
            DateTime.now(),
            VelocityTracker.withKind(_pointers.values.first.downEvent.kind),
          );
          if (claimVictoryOnStart) {
            _winAll();
          }
        }
      case final ThreeFingerStateActive active:
        final delta = centroid - active.lastCentroid;
        if (onSwipe != null) {
          invokeCallback('onSwipe', () => onSwipe!(delta));
        }
        active
          ..lastCentroid = centroid
          ..velocityTracker.addPosition(
            DateTime.now().difference(active.startTime),
            centroid,
          );
      case ThreeFingerStateIdle():
        break;
    }
  }

  // Ends the gesture, firing onSwipeEnd if it was active, and blocks re-arming
  // until every pointer has lifted.
  void _end(Offset? centroid) {
    final state = swipeState;
    if (state is ThreeFingerStateActive && onSwipeEnd != null) {
      final velocity = state.velocityTracker.getVelocityEstimate();
      final position = centroid ?? state.lastCentroid;
      invokeCallback('onSwipeEnd', () => onSwipeEnd!(position, velocity));
    }
    if (state is! ThreeFingerStateIdle) {
      swipeState = const ThreeFingerStateIdle();
      _abandoned = true;
    }
  }

  void _untrack(int pointer) {
    GestureBinding.instance.pointerRouter.removeRoute(pointer, _handleEvent);
    _pointers.remove(pointer);
    if (_pointers.isEmpty) {
      _abandoned = false;
    }
  }

  void _winAll() {
    for (final tracker in _pointers.values) {
      tracker.win();
    }
  }

  @override
  void acceptGesture(int pointer) {
    _pointers[pointer]?.winSkip();
  }

  @override
  void rejectGesture(int pointer) {
    if (_pointers[pointer] == null) {
      return;
    }
    _pointers[pointer]!.loseSkip();
    _end(_centroid());
  }

  @override
  void dispose() {
    for (final tracker in _pointers.values.toList()) {
      GestureBinding.instance.pointerRouter
          .removeRoute(tracker.pointer, _handleEvent);
      tracker.lose();
    }
    _pointers.clear();
    super.dispose();
  }

  // coverage:ignore-start
  @override
  void debugFillProperties(DiagnosticPropertiesBuilder properties) {
    super.debugFillProperties(properties);
    properties
      ..add(DiagnosticsProperty('swipeState', swipeState))
      ..add(DiagnosticsProperty('claimVictoryOnStart', claimVictoryOnStart))
      ..add(DiagnosticsProperty('slop', slop))
      ..add(ObjectFlagProperty.has('onSwipe', onSwipe))
      ..add(ObjectFlagProperty.has('onSwipeEnd', onSwipeEnd));
  }
  // coverage:ignore-end
}

/// State of a single pointer with respect to the gesture arena.
enum _TrackerState { won, lost, pending }

/// Tracks one pointer's arena entry and latest position.
class _Tracker {
  _Tracker(this.downEvent, this._entry, this._state) : _lastEvent = downEvent;

  final PointerDownEvent downEvent;
  PointerEvent _lastEvent;
  GestureArenaEntry? _entry;
  _TrackerState _state;

  int get pointer => downEvent.pointer;
  Offset get position => _lastEvent.localPosition;

  void win() {
    if (_state == _TrackerState.pending) {
      _entry?.resolve(GestureDisposition.accepted);
    }
    _entry = null;
    _state = _TrackerState.won;
  }

  void lose() {
    if (_state == _TrackerState.pending) {
      _entry?.resolve(GestureDisposition.rejected);
    }
    _entry = null;
    _state = _TrackerState.lost;
  }

  void winSkip() {
    _entry = null;
    _state = _TrackerState.won;
  }

  void loseSkip() {
    _entry = null;
    _state = _TrackerState.lost;
  }
}

/// Activation state of the three-finger swipe.
sealed class ThreeFingerState {
  const ThreeFingerState();
}

/// Fewer than three pointers are down, or the gesture has ended.
class ThreeFingerStateIdle extends ThreeFingerState {
  const ThreeFingerStateIdle();
}

/// Three pointers are down; waiting for the centroid to clear the slop.
class ThreeFingerStateWaitingForSlop extends ThreeFingerState {
  const ThreeFingerStateWaitingForSlop(this.startCentroid);
  final Offset startCentroid;
}

/// The swipe is active and reporting centroid deltas.
class ThreeFingerStateActive extends ThreeFingerState {
  ThreeFingerStateActive(
    this.lastCentroid,
    this.startTime,
    this.velocityTracker,
  );
  Offset lastCentroid;
  final DateTime startTime;
  final VelocityTracker velocityTracker;
}
