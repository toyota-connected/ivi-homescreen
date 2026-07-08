import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';

import '_double_finger_gesture_recognizer.dart';

///
typedef GesturePitchCallback = void Function(double verticalDelta);

/// Tracks a pitch gesture state. Callbacks are called each time one of the two
/// pointers move.
///
/// `verticalDelta` is the difference in pixels of the y values of the center
/// point between the two pointers before and after the move event.
class PitchGestureRecognizer extends DoubleFingerGestureRecognizer {
  ///
  PitchGestureRecognizer({
    this.slop = 0,
    double tolerance = 180,
    this.claimVictoryOnStart = false,
    super.supportedDevices,
  })  : assert(tolerance > 0 && tolerance <= 180),
        tolerance = tolerance * pi / 180;
  @override
  String get debugDescription => 'Pitch';

  ///
  GesturePitchCallback? onPitch;

  ///The vertical distance that the center point between the two fingers
  ///must first pass in order to start the gesture.
  final double slop;

  ///The absolute angle offset in degrees from the x axis of
  ///the line drawn between the two pressed fingers which, if exceeded at any
  ///point, the gesture is cancelled.
  final double tolerance;

  ///Whether the gesture claims exclusive ownership of its pointers once
  ///slops are exceeded and the gesture begins.
  final bool claimVictoryOnStart;

  ///Provides the current state the gesture, containing info about the
  ///activation state of the gesture.
  PitchState pitchState = const PitchStateIdle();

  ///Determines if the given snapshot is within tolerance
  bool exceedsTolerance(PointerSnapshot snapshot) =>
      snapshot.angle.abs() > tolerance;

  ///Checks if gesture is waiting for slop to be exceeded, and if it is,
  ///transitions to active state.
  void checkSlop(PointerSnapshot snapshot) {
    final pitchState = this.pitchState;
    if (pitchState is PitchStateWaitingForSlop) {
      final next = pitchState.tryAdvance(snapshot, slop: slop);
      if (next != null) {
        this.pitchState = next;
        if (claimVictoryOnStart) pointerState.win();
      }
    }
  }

  @override
  void onUpdate(PointerState previous, PointerState curr) {
    if (curr is PointerStateTwoFingers && exceedsTolerance(curr.snapshot)) {
      curr.lose();
    } else {
      final pitchState = this.pitchState;
      if (previous is! PointerStateTwoFingers &&
          curr is PointerStateTwoFingers &&
          pitchState is PitchStateIdle) {
        this.pitchState = pitchState.advance(curr);
        checkSlop(curr.snapshot);
      } else if (previous is! PointerStateIdle && curr is PointerStateIdle) {
        this.pitchState = const PitchStateIdle();
      }
    }
  }

  @override
  void onMove(PointerMoveEvent event) {
    final pointerState = this.pointerState;
    if (pointerState is! PointerStateTwoFingers) {
      return;
    }
    final currSnapshot = pointerState.snapshot;
    if (exceedsTolerance(currSnapshot)) {
      pointerState.lose();
      return;
    }
    switch (pitchState) {
      case final PitchStateActive active:
        if (onPitch != null) {
          final verticalDelta = active.verticalDelta(currSnapshot);
          invokeCallback('onPitch', () => onPitch!(verticalDelta));
        }
        active.lastSnapshot = currSnapshot;
      case PitchStateIdle():
      case PitchStateWaitingForSlop():
        checkSlop(currSnapshot);
    }
  }

  // coverage:ignore-start
  @override
  void debugFillProperties(DiagnosticPropertiesBuilder properties) {
    super.debugFillProperties(properties);
    properties
      ..add(DiagnosticsProperty('pitchState', pitchState))
      ..add(DiagnosticsProperty('claimVictoryOnStart', claimVictoryOnStart))
      ..add(DiagnosticsProperty('tolerance', tolerance))
      ..add(DiagnosticsProperty('slop', slop))
      ..add(ObjectFlagProperty.has('onPitch', onPitch));
  }
  // coverage:ignore-end
}

sealed class PitchState {
  const PitchState();
}

class PitchStateIdle extends PitchState {
  const PitchStateIdle();

  PitchStateWaitingForSlop advance(PointerStateTwoFingers pointerState) =>
      PitchStateWaitingForSlop(pointerState.snapshot);
}

class PitchStateWaitingForSlop extends PitchState {
  const PitchStateWaitingForSlop(this.startSnapshot);
  final PointerSnapshot startSnapshot;

  PitchStateActive? tryAdvance(
    PointerSnapshot currSnapshot, {
    required double slop,
  }) =>
      (currSnapshot.center - startSnapshot.center).dy.abs() >= slop
          ? PitchStateActive(currSnapshot)
          : null;
}

class PitchStateActive extends PitchState {
  PitchStateActive(this.lastSnapshot);
  PointerSnapshot lastSnapshot;

  double verticalDelta(PointerSnapshot currSnapshot) =>
      (currSnapshot.center - lastSnapshot.center).dy;
}
