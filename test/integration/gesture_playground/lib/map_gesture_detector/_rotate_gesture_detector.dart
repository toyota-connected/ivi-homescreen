import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';

import '_double_finger_gesture_recognizer.dart';

typedef GestureRotateCallback = void Function(double angleDelta);

class RotateGestureRecognizer extends DoubleFingerGestureRecognizer {
  ///Slop is the angle in degrees difference between two lines drawn between
  ///pointers first at the moment where two are both pressed, and second at
  ///current moment that must be cleared before the gesture becomes active.
  RotateGestureRecognizer({
    double slop = 0,
    this.claimVictoryOnStart = false,
    super.supportedDevices,
  })  : assert(slop >= 0 && slop <= 180),
        slop = slop * pi / 180;
  @override
  String get debugDescription => 'Rotate';

  ///relativeAngle is reported in degrees.
  GestureRotateCallback? onRotate;

  final double slop;

  ///Whether the gesture claims exclusive ownership of its pointers once
  ///slops are exceeded and the gesture begins.
  final bool claimVictoryOnStart;

  ///Provides the current state the gesture, containing info about the
  ///activation state of the gesture.
  RotateState rotateState = const RotateStateIdle();

  ///Checks if gesture is waiting for slop to be exceeded, and if it is,
  ///transitions to active state.
  void checkSlop(PointerSnapshot snapshot) {
    final rotateState = this.rotateState;
    if (rotateState is RotateStateWaitingForSlop) {
      final next = rotateState.tryAdvance(snapshot, slop: slop);
      if (next != null) {
        this.rotateState = next;
        if (claimVictoryOnStart) pointerState.win();
      }
    }
  }

  @override
  void onUpdate(PointerState previous, PointerState curr) {
    final rotateState = this.rotateState;
    if (previous is! PointerStateTwoFingers &&
        curr is PointerStateTwoFingers &&
        rotateState is RotateStateIdle) {
      this.rotateState = rotateState.advance(curr);
      checkSlop(curr.snapshot);
    } else if (previous is! PointerStateIdle && curr is PointerStateIdle) {
      this.rotateState = const RotateStateIdle();
    }
  }

  @override
  void onMove(PointerMoveEvent event) {
    final pointerState = this.pointerState;
    if (pointerState is! PointerStateTwoFingers) {
      return;
    }
    switch (rotateState) {
      case final RotateStateActive active:
        final currSnapshot = pointerState.snapshot;
        if (onRotate != null) {
          final angleDelta = active.angleDelta(currSnapshot);
          invokeCallback('onRotate', () => onRotate!(angleDelta));
        }
        active.lastSnapshot = currSnapshot;
      case RotateStateWaitingForSlop():
        checkSlop(pointerState.snapshot);
      case RotateStateIdle():
        break;
    }
  }

  // coverage:ignore-start
  @override
  void debugFillProperties(DiagnosticPropertiesBuilder properties) {
    super.debugFillProperties(properties);
    properties
      ..add(DiagnosticsProperty('rotateState', rotateState))
      ..add(DiagnosticsProperty('claimVictoryOnStart', claimVictoryOnStart))
      ..add(DiagnosticsProperty('slop', slop))
      ..add(ObjectFlagProperty.has('onRotate', onRotate));
  }
  // coverage:ignore-end
}

sealed class RotateState {
  const RotateState();
}

class RotateStateIdle extends RotateState {
  const RotateStateIdle();

  RotateStateWaitingForSlop advance(PointerStateTwoFingers pointerState) =>
      RotateStateWaitingForSlop(pointerState.snapshot);
}

class RotateStateWaitingForSlop extends RotateState {
  const RotateStateWaitingForSlop(this.startSnapshot);
  final PointerSnapshot startSnapshot;

  RotateStateActive? tryAdvance(
    PointerSnapshot currSnapshot, {
    required double slop,
  }) =>
      (currSnapshot.angle - startSnapshot.angle).abs() >= slop
          ? RotateStateActive(currSnapshot)
          : null;
}

class RotateStateActive extends RotateState {
  RotateStateActive(this.lastSnapshot);
  PointerSnapshot lastSnapshot;

  double angleDelta(PointerSnapshot currSnapshot) {
    final lastAngle = lastSnapshot.angle;
    //radian to degrees, clockwise
    return (currSnapshot.angle - lastAngle) * 180 / pi;
  }
}
