import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';

/// This class manages all of the gesture arena calls and callbacks for a child
/// gesture recognizer that wants to track one or two finger gestures.
///
/// `onMove`, `onUp`, and `onCancel` callbacks are only called when the gesture
/// is still active, meaning the child class has not manually "lost" the first
/// or second pointers, or another gesture has claimed victory over the first or
/// second pointer.
///
/// `onUpdate` callback can be used to track the lifecycle of the current
/// gesture/pointer state.
///
/// This class only concerns itself with the state of the pointers, not the
/// actual gesture, which should be handled by the child class.
abstract class DoubleFingerGestureRecognizer extends GestureRecognizer {
  ///
  DoubleFingerGestureRecognizer({
    this.loseWhenMoreThanTwoPointers = false,
    super.supportedDevices,
  });

  ///Whether to automatically stop the gesture when a third pointer is added
  final bool loseWhenMoreThanTwoPointers;

  final Map<int, PointerTracker> _currentPointers = {};
  PointerState _pointerState = const PointerStateIdle();

  ///Provides the current state the gesture, containing info about the
  ///positions of first two pointers.
  PointerState get pointerState => _pointerState;

  /// Override to listen to pointer state changes including:
  /// - When the first pointer taps down
  /// - When the second pointer taps down
  /// - When the gesture is canceled either due to the child class manually
  /// "losing" the pointer(s), or due to another gesture recognizer claiming
  /// victory over the first or second pointer.
  void onUpdate(PointerState previous, PointerState curr);

  /// Override to be notified when either the first or second pointer moves.
  ///
  /// Only called when pointers are active/not lost.
  void onMove(PointerMoveEvent event) {}

  /// Override to be notified when either the first or second pointer is
  /// released. After this callback, the gesture will terminate and an
  /// `onUpdate` call will be sent with `PointerStateIdle`.
  ///
  /// Only called when pointers are active/not lost.
  void onUp(PointerUpEvent event) {}

  /// Override to be notified when either the first or second pointer is
  /// cancelled. After this callback, the gesture will terminate and an
  /// `onUpdate` call will be sent with `PointerStateIdle`.
  ///
  /// Only called when pointers are active/not lost.
  void onCancel(PointerCancelEvent event) {}

  void _update(PointerState nextState) {
    if (pointerState != nextState) {
      final prevState = _pointerState;
      _pointerState = nextState;
      onUpdate(prevState, nextState);
    }
  }

  void _reset() {
    pointerState.lose();
    _update(const PointerStateIdle());
  }

  @override
  void dispose() {
    pointerState.lose();
    super.dispose();
  }

  void _startTracking(PointerTracker tracker) {
    GestureBinding.instance.pointerRouter
        .addRoute(tracker.pointer, _onUpdate, tracker.downEvent.transform);
    _currentPointers[tracker.pointer] = tracker;
  }

  void _stopTracking(PointerTracker tracker) {
    _currentPointers.remove(tracker.pointer)!;
    GestureBinding.instance.pointerRouter
        .removeRoute(tracker.pointer, _onUpdate);
  }

  @override
  void addAllowedPointer(PointerDownEvent event) {
    super.addAllowedPointer(event);

    final PointerTracker tracker;

    final pointerState = this.pointerState;
    if (_currentPointers.isEmpty && pointerState is PointerStateIdle) {
      final entry =
          GestureBinding.instance.gestureArena.add(event.pointer, this);
      tracker = PointerTracker(event, entry, TrackerState.pending);
      _startTracking(tracker);
      _update(pointerState.addFinger(tracker));
    } else if (_currentPointers.length == 1 &&
        pointerState is PointerStateOneFinger) {
      final entry =
          GestureBinding.instance.gestureArena.add(event.pointer, this);
      tracker = PointerTracker(event, entry, TrackerState.pending);
      _startTracking(tracker);
      _update(pointerState.addFinger(tracker));
    } else {
      tracker = PointerTracker(event, null, TrackerState.lost);
      _startTracking(tracker);
      if (loseWhenMoreThanTwoPointers) _reset();
    }
  }

  void _onUpdate(PointerEvent event) {
    final tracker = _currentPointers[event.pointer];
    if (tracker != null) {
      if (event is PointerUpEvent) {
        tracker._lastEvent = event;
        if (pointerState.includes(event.pointer)) {
          onUp(event);
          _reset();
        }
        _stopTracking(tracker);
      } else if (event is PointerCancelEvent) {
        tracker._lastEvent = event;
        if (pointerState.includes(event.pointer)) {
          onCancel(event);
          _reset();
        }
        _stopTracking(tracker);
      } else if (event is PointerMoveEvent) {
        tracker._lastEvent = event;
        if (pointerState.includes(event.pointer)) {
          onMove(event);
        }
      }
    }
  }

  @override
  void acceptGesture(int pointer) {
    _currentPointers[pointer]?._win(skipResolution: true);
  }

  @override
  void rejectGesture(int pointer) {
    _currentPointers[pointer]?._lose(skipResolution: true);
    if (pointerState.includes(pointer)) {
      _reset();
    }
  }

  // coverage:ignore-start
  @override
  void debugFillProperties(DiagnosticPropertiesBuilder properties) {
    super.debugFillProperties(properties);
    properties
      ..add(DiagnosticsProperty('pointerState', pointerState))
      ..add(
        DiagnosticsProperty(
          'loseWhenMoreThanTwoPointers',
          loseWhenMoreThanTwoPointers,
        ),
      );
  }
  // coverage:ignore-end
}

/// Represents the state of a single pointer
class PointerTracker {
  ///
  PointerTracker(this.downEvent, this._entry, this._state)
      : _lastEvent = downEvent;

  /// The initial down event for the pointer
  final PointerDownEvent downEvent;
  PointerEvent _lastEvent;
  GestureArenaEntry? _entry;
  TrackerState _state;

  /// Initially the same as `downEvent`, but then updates to
  /// the latest `PointerMoveEvent` (if any), then finally
  /// either `PointerUpEvent` or `PointerCancelEvent`.
  PointerEvent get lastEvent => _lastEvent;

  ///Shortcut to getting pointer ID
  int get pointer => downEvent.pointer;

  /// Signals a manual victory over the pointer. If the pointer is already
  /// won or lost, no other effect will happen.
  void win() => _win(skipResolution: false);
  void _win({required bool skipResolution}) {
    if (!skipResolution && _state == TrackerState.pending) {
      _entry?.resolve(GestureDisposition.accepted);
    }
    _entry = null;
    _state = TrackerState.won;
    //print('win: ${downEvent.pointer}');
  }

  /// Signals a manual loss over the pointer. If the pointer is already
  /// won or lost, no other effect will happen.
  void lose() => _lose(skipResolution: false);
  void _lose({required bool skipResolution}) {
    if (!skipResolution && _state == TrackerState.pending) {
      _entry?.resolve(GestureDisposition.rejected);
    }
    _entry = null;
    _state = TrackerState.lost;
    //print('lose: ${downEvent.pointer}');
  }
}

/// State of the pointer with respect to a gesture.
enum TrackerState {
  ///
  won,

  ///
  lost,

  ///
  pending,
}

/// Represents the pointer state of the double finger gesture recognizer.
/// It tracks how many pointers are active, and their initial and latest events.
sealed class PointerState {
  ///
  const PointerState();

  /// Whether any of the active pointers have a pointer ID.
  bool includes(int pointer);

  /// Manually loses any active pointers.
  void lose();

  /// Manually wins any active pointers.
  void win();
}

/// Indicates no pointers are active.
class PointerStateIdle extends PointerState {
  ///
  const PointerStateIdle();

  @override
  bool includes(int pointer) => false;
  @override
  void lose() {}
  @override
  void win() {}

  /// Shortcut to transition to the next state in the sequence
  PointerStateOneFinger addFinger(PointerTracker first) =>
      PointerStateOneFinger(first);
}

/// Indicates one pointer is active.
class PointerStateOneFinger extends PointerState {
  ///
  const PointerStateOneFinger(this.first);

  ///
  final PointerTracker first;

  @override
  bool includes(int pointer) => first.pointer == pointer;

  @override
  void lose() => first.lose();

  @override
  void win() => first.win();

  /// Shortcut to transition to the next state in the sequence
  PointerStateTwoFingers addFinger(PointerTracker second) =>
      PointerStateTwoFingers(
        first,
        second,
        PointerSnapshot._(
          first._lastEvent,
          second._lastEvent,
        ),
      );
}

/// Indicates two or more (if `loseWhenMoreThanTwoPointers` is false) pointers
/// are active.
class PointerStateTwoFingers extends PointerState {
  ///
  const PointerStateTwoFingers(this.first, this.second, this.startSnapshot);

  ///
  final PointerTracker first;

  ///
  final PointerTracker second;

  ///Snapshot at the moment where the second pointer was pressed
  final PointerSnapshot startSnapshot;

  @override
  bool includes(int pointer) =>
      first.pointer == pointer || second.pointer == pointer;

  @override
  void lose() {
    first.lose();
    second.lose();
  }

  @override
  void win() {
    first.win();
    second.win();
  }

  /// Retreives the current snapshot of the two pointers.
  PointerSnapshot get snapshot => PointerSnapshot._(
        first._lastEvent,
        second._lastEvent,
      );
}

/// Represents a snapshot of the latest positions of the two pointers.
class PointerSnapshot {
  PointerSnapshot._(this.first, this.second);

  ///
  final PointerEvent first;

  ///
  final PointerEvent second;

  /// Distance between the two pointers.
  double get distance => (second.localPosition - first.localPosition).distance;

  /// Center point between the two pointers.
  Offset get center {
    final firstPosition = first.localPosition;
    final secondPosition = second.localPosition;
    return Offset(
      firstPosition.dx + (secondPosition.dx - firstPosition.dx) / 2,
      firstPosition.dy + (secondPosition.dy - firstPosition.dy) / 2,
    );
  }

  /// Returns angle of the line between the two pointers in radians clockwise
  /// from positive x axis. Range: -pi (exclusive) to pi (inclusive)
  double get angle => (second.localPosition - first.localPosition).direction;
}
