import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';

import '_double_finger_gesture_recognizer.dart';

typedef GestureScrollCallback = void Function(Offset delta);
typedef GestureScrollEndCallback = void Function(
  Offset position,
  VelocityEstimate? velocityEstimate,
);

class ScrollGestureRecognizer extends DoubleFingerGestureRecognizer {
  ///When only one figer is down, slop is the distance between the initial down
  ///and current position of the finger that must pass in order to start the
  ///gesture.
  ///
  ///When two fingers are down, and gesture has not started yet, slop is also
  ///the distance that the center point between the two fingers must pass in
  ///order to start the gesture.
  ScrollGestureRecognizer({
    this.slop = 0,
    this.claimVictoryOnStart = false,
    super.supportedDevices,
  });
  @override
  String get debugDescription => 'Scroll';

  GestureScrollCallback? onScroll;
  GestureScrollEndCallback? onScrollEnd;

  final double slop;

  ///Whether the gesture claims exclusive ownership of its pointers once
  ///slops are exceeded and the gesture begins.
  final bool claimVictoryOnStart;

  ///Provides the current state the gesture, containing info about the
  ///activation state of the gesture.
  ScrollState scrollState = const ScrollStateIdle();

  ///Checks if gesture is waiting for slop to be exceeded, and if it is,
  ///transitions to active state.
  void checkSlop(PointerState pointerState) {
    final scrollState = this.scrollState;
    ScrollState? next;

    if (scrollState is ScrollStateSingleWaitingForSlop &&
        pointerState is PointerStateOneFinger) {
      next = scrollState.tryAdvanceSingle(pointerState.first, slop: slop);
    } else if (scrollState is ScrollStateDoubleWaitingForSlop &&
        pointerState is PointerStateTwoFingers) {
      next = scrollState.tryAdvance(pointerState.snapshot, slop: slop);
    }

    if (next != null) {
      this.scrollState = next;
      if (claimVictoryOnStart) pointerState.win();
    }
  }

  @override
  void onUpdate(PointerState previous, PointerState curr) {
    final scrollState = this.scrollState;
    if (scrollState is ScrollStateIdle &&
        previous is! PointerStateOneFinger &&
        curr is PointerStateOneFinger) {
      this.scrollState = scrollState.advance(curr.first);
      checkSlop(curr);
    } else if (previous is! PointerStateTwoFingers &&
        curr is PointerStateTwoFingers) {
      if (scrollState is ScrollStateSingle) {
        this.scrollState = scrollState.toDoubleFinger(curr.snapshot);
      } else if (scrollState is ScrollStateSingleWaitingForSlop) {
        this.scrollState = scrollState.toDoubleFinger(curr.snapshot);
      }
    } else if (previous is! PointerStateIdle && curr is PointerStateIdle) {
      this.scrollState = const ScrollStateIdle();
    }
  }

  @override
  void onUp(PointerUpEvent event) {
    final pointerState = this.pointerState;
    if (onScrollEnd != null) {
      switch (scrollState) {
        case ScrollStateSingle(:final velocityTracker):
          final velocityEstimate = velocityTracker.getVelocityEstimate();
          invokeCallback(
            'onScrollEnd',
            () => onScrollEnd!(
              event.localPosition,
              velocityEstimate,
            ),
          );
        case ScrollStateDouble(:final velocityTracker)
            when pointerState is PointerStateTwoFingers:
          final center = pointerState.snapshot.center;
          final velocityEstimate = velocityTracker.getVelocityEstimate();
          invokeCallback(
            'onScrollEnd',
            () => onScrollEnd!(center, velocityEstimate),
          );
        case ScrollStateIdle():
        case ScrollStateSingleWaitingForSlop():
        case ScrollStateDoubleWaitingForSlop():
        case ScrollStateDouble():
          break;
      }
    }
    super.onUp(event);
  }

  @override
  void onMove(PointerMoveEvent event) {
    final pointerState = this.pointerState;
    final scrollState = this.scrollState;
    if (pointerState is PointerStateOneFinger &&
        scrollState is ScrollStateSingle) {
      if (onScroll != null) {
        invokeCallback(
          'onScroll',
          () => onScroll!(event.localDelta),
        );
      }
      scrollState.update(pointerState.first);
    } else if (pointerState is PointerStateTwoFingers &&
        scrollState is ScrollStateDouble) {
      final currSnapshot = pointerState.snapshot;
      final scrollDelta = scrollState.delta(currSnapshot);
      if (onScroll != null) {
        invokeCallback(
          'onScroll',
          () => onScroll!(scrollDelta),
        );
      }
      scrollState.update(currSnapshot);
    } else {
      checkSlop(pointerState);
    }
  }

  // coverage:ignore-start
  @override
  void debugFillProperties(DiagnosticPropertiesBuilder properties) {
    super.debugFillProperties(properties);
    properties
      ..add(DiagnosticsProperty('scrollState', scrollState))
      ..add(DiagnosticsProperty('claimVictoryOnStart', claimVictoryOnStart))
      ..add(DiagnosticsProperty('slop', slop))
      ..add(ObjectFlagProperty.has('onScrollEnd', onScrollEnd))
      ..add(ObjectFlagProperty.has('onScroll', onScroll));
  }
  // coverage:ignore-end
}

sealed class ScrollState {
  const ScrollState();
}

class ScrollStateIdle extends ScrollState {
  const ScrollStateIdle();

  ScrollStateSingleWaitingForSlop advance(
    PointerTracker tracker,
  ) =>
      const ScrollStateSingleWaitingForSlop();
}

class ScrollStateSingleWaitingForSlop extends ScrollState {
  const ScrollStateSingleWaitingForSlop();

  ScrollStateSingle? tryAdvanceSingle(
    PointerTracker tracker, {
    required double slop,
  }) =>
      (tracker.lastEvent.localPosition - tracker.downEvent.localPosition)
                  .distance >=
              slop
          ? ScrollStateSingle(
              DateTime.now(),
              VelocityTracker.withKind(tracker.downEvent.kind),
            )
          : null;

  ScrollStateDoubleWaitingForSlop toDoubleFinger(
    PointerSnapshot startSnapshot,
  ) =>
      ScrollStateDoubleWaitingForSlop(startSnapshot);
}

class ScrollStateSingle extends ScrollState {
  const ScrollStateSingle(this.startTime, this.velocityTracker);

  final DateTime startTime;
  final VelocityTracker velocityTracker;

  ScrollStateDouble toDoubleFinger(PointerSnapshot lastSnapshot) =>
      ScrollStateDouble(
        lastSnapshot,
        DateTime.now(),
        VelocityTracker.withKind(lastSnapshot.second.kind),
      );

  void update(PointerTracker tracker) {
    velocityTracker.addPosition(
      DateTime.now().difference(startTime),
      tracker.lastEvent.localPosition,
    );
  }
}

class ScrollStateDoubleWaitingForSlop extends ScrollState {
  const ScrollStateDoubleWaitingForSlop(this.startSnapshot);
  final PointerSnapshot startSnapshot;

  ScrollStateDouble? tryAdvance(
    PointerSnapshot currSnapshot, {
    required double slop,
  }) =>
      (currSnapshot.center - startSnapshot.center).distance >= slop
          ? ScrollStateDouble(
              currSnapshot,
              DateTime.now(),
              VelocityTracker.withKind(startSnapshot.second.kind),
            )
          : null;
}

class ScrollStateDouble extends ScrollState {
  ScrollStateDouble(
    this.lastSnapshot,
    this.startTime,
    this.velocityTracker,
  );
  PointerSnapshot lastSnapshot;
  final DateTime startTime;
  final VelocityTracker velocityTracker;

  Offset delta(PointerSnapshot currSnapshot) =>
      currSnapshot.center - lastSnapshot.center;

  void update(PointerSnapshot nextSnapshot) {
    lastSnapshot = nextSnapshot;
    velocityTracker.addPosition(
      DateTime.now().difference(startTime),
      nextSnapshot.center,
    );
  }
}
