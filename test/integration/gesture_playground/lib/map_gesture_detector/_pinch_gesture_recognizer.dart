import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';

import '_double_finger_gesture_recognizer.dart';

///
typedef GesturePinchCallback = void Function(Offset center, double scaleDelta);

/// Tracks a pinch gesture state. Callbacks are called each time one of the two
/// pointers move.
///
/// `scaleDelta` is the percent difference (not pixels) between the size of the
/// line drawn between the two pointers before and after the move event.
class PinchGestureRecognizer extends DoubleFingerGestureRecognizer {
  ///
  PinchGestureRecognizer({
    this.slop = 0,
    this.claimVictoryOnStart = false,
    super.supportedDevices,
  });
  @override
  String get debugDescription => 'Pinch';

  ///
  GesturePinchCallback? onPinch;

  ///Slop is the absolute difference between the initial starting distance and
  ///the current distance between the two pointers that must be exceeded at
  ///least once in order to start the gesture.
  final double slop;

  ///Whether the gesture claims exclusive ownership of its pointers once
  ///slops are exceeded and the gesture begins.
  final bool claimVictoryOnStart;

  ///Provides the current state the gesture, containing info about the
  ///activation state of the gesture.
  PinchState pinchState = const PinchStateIdle();

  ///Checks if gesture is waiting for slop to be exceeded, and if it is,
  ///transitions to active state.
  void checkSlop(PointerSnapshot snapshot) {
    final pinchState = this.pinchState;
    if (pinchState is PinchStateWaitingForSlop) {
      final next = pinchState.tryAdvance(snapshot, slop: slop);
      if (next != null) {
        this.pinchState = next;
        if (claimVictoryOnStart) pointerState.win();
      }
    }
  }

  @override
  void onUpdate(PointerState previous, PointerState curr) {
    final pinchState = this.pinchState;
    if (previous is! PointerStateTwoFingers &&
        curr is PointerStateTwoFingers &&
        pinchState is PinchStateIdle) {
      this.pinchState = pinchState.advance(curr);
      checkSlop(curr.snapshot);
    } else if (previous is! PointerStateIdle && curr is PointerStateIdle) {
      this.pinchState = const PinchStateIdle();
    }
  }

  @override
  void onMove(PointerMoveEvent event) {
    final pointerState = this.pointerState;
    if (pointerState is! PointerStateTwoFingers) {
      return;
    }
    switch (pinchState) {
      case final PinchStateActive active:
        final currSnapshot = pointerState.snapshot;
        if (onPinch != null) {
          final scaleDelta = active.scaleDelta(currSnapshot);
          invokeCallback(
            'onPinch',
            () => onPinch!(currSnapshot.center, scaleDelta),
          );
        }
        active.lastSnapshot = currSnapshot;
      case PinchStateIdle():
      case PinchStateWaitingForSlop():
        checkSlop(pointerState.snapshot);
    }
  }

  // coverage:ignore-start
  @override
  void debugFillProperties(DiagnosticPropertiesBuilder properties) {
    super.debugFillProperties(properties);
    properties
      ..add(DiagnosticsProperty('pinchState', pinchState))
      ..add(DiagnosticsProperty('claimVictoryOnStart', claimVictoryOnStart))
      ..add(DiagnosticsProperty('slop', slop))
      ..add(ObjectFlagProperty.has('onPinch', onPinch));
  }
  // coverage:ignore-end
}

sealed class PinchState {
  const PinchState();
}

class PinchStateIdle extends PinchState {
  const PinchStateIdle();

  PinchStateWaitingForSlop advance(PointerStateTwoFingers pointerState) =>
      PinchStateWaitingForSlop(pointerState.snapshot);
}

class PinchStateWaitingForSlop extends PinchState {
  const PinchStateWaitingForSlop(this.startSnapshot);
  final PointerSnapshot startSnapshot;

  PinchStateActive? tryAdvance(
    PointerSnapshot currSnapshot, {
    required double slop,
  }) =>
      (currSnapshot.distance - startSnapshot.distance).abs() >= slop
          ? PinchStateActive(currSnapshot)
          : null;
}

class PinchStateActive extends PinchState {
  PinchStateActive(this.lastSnapshot);
  PointerSnapshot lastSnapshot;

  double scaleDelta(PointerSnapshot currSnapshot) {
    final lastDistance = lastSnapshot.distance;
    return (currSnapshot.distance - lastDistance) / lastDistance;
  }
}
