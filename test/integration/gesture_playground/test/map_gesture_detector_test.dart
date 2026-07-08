// ignore_for_file: diagnostic_describe_all_properties

import 'dart:math';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:gesture_playground/map_gesture_detector/_double_finger_gesture_recognizer.dart';
import 'package:gesture_playground/map_gesture_detector/_pinch_gesture_recognizer.dart';
import 'package:gesture_playground/map_gesture_detector/_pitch_gesture_recognizer.dart';
import 'package:gesture_playground/map_gesture_detector/_rotate_gesture_detector.dart';
import 'package:gesture_playground/map_gesture_detector/_scroll_gesture_recognizer.dart';
import 'package:gesture_playground/map_gesture_detector/_three_finger_swipe_gesture_recognizer.dart';

//Needs to be static so that all newly added pointers are unique.
int nextPointer = 1813534;

void main() {
  group(
    'double finger gesture recognizer tests',
    _doubleFingerGestureRecognizerTests,
  );
  group('pinch gesture recognizer tests', _pinchGestureRecognizerTests);
  group('rotate gesture recognizer tests', _rotateGestureRecognizerTests);
  group('pitch gesture recognizer tests', _pitchGestureRecognizerTests);
  group('scroll gesture recognizer tests', _scrollGestureRecognizerTests);
  group(
    'three finger swipe gesture recognizer tests',
    _threeFingerSwipeGestureRecognizerTests,
  );
}

void _threeFingerSwipeGestureRecognizerTests() {
  Future<ThreeFingerSwipeGestureRecognizer> pumpRecognizer(
    WidgetTester tester, {
    required List<Offset> deltas,
    required List<Offset> endPositions,
  }) async {
    final recognizer = ThreeFingerSwipeGestureRecognizer()
      ..onSwipe = deltas.add
      ..onSwipeEnd = (position, velocity) => endPositions.add(position);
    await tester.pumpWidget(
      RawGestureDetector(
        behavior: HitTestBehavior.opaque,
        gestures: <Type, GestureRecognizerFactory>{
          ThreeFingerSwipeGestureRecognizer:
              GestureRecognizerFactoryWithHandlers<
                  ThreeFingerSwipeGestureRecognizer>(
            () => recognizer,
            (ThreeFingerSwipeGestureRecognizer instance) {},
          ),
        },
        child: const SizedBox(width: 200, height: 200),
      ),
    );
    return recognizer;
  }

  testWidgets('arms on the third pointer and reports centroid delta',
      (WidgetTester tester) async {
    final deltas = <Offset>[];
    final endPositions = <Offset>[];
    final recognizer = await pumpRecognizer(
      tester,
      deltas: deltas,
      endPositions: endPositions,
    );

    assert(recognizer.swipeState is ThreeFingerStateIdle);
    final g1 = await tester.startGesture(
      const Offset(50, 50),
      pointer: nextPointer++,
    );
    final g2 = await tester.startGesture(
      const Offset(150, 50),
      pointer: nextPointer++,
    );
    assert(recognizer.swipeState is ThreeFingerStateIdle);
    final g3 = await tester.startGesture(
      const Offset(100, 140),
      pointer: nextPointer++,
    );
    assert(recognizer.swipeState is ThreeFingerStateWaitingForSlop);

    // First qualifying move (slop 0) activates without emitting.
    await g1.moveBy(const Offset(30, 0));
    assert(recognizer.swipeState is ThreeFingerStateActive);
    assert(deltas.isEmpty);

    // Moving one finger by 30 shifts the 3-pointer centroid by 10.
    await g1.moveBy(const Offset(30, 0));
    assert(deltas.length == 1);
    assert(deltas.last == const Offset(10, 0));

    await g1.up();
    assert(recognizer.swipeState is ThreeFingerStateIdle);
    assert(endPositions.length == 1);

    await g2.up();
    await g3.up();
  });

  testWidgets('a fourth pointer ends the gesture', (WidgetTester tester) async {
    final deltas = <Offset>[];
    final endPositions = <Offset>[];
    final recognizer = await pumpRecognizer(
      tester,
      deltas: deltas,
      endPositions: endPositions,
    );

    final g1 = await tester.startGesture(
      const Offset(50, 50),
      pointer: nextPointer++,
    );
    final g2 = await tester.startGesture(
      const Offset(150, 50),
      pointer: nextPointer++,
    );
    final g3 = await tester.startGesture(
      const Offset(100, 140),
      pointer: nextPointer++,
    );
    await g1.moveBy(const Offset(30, 0));
    assert(recognizer.swipeState is ThreeFingerStateActive);

    final g4 = await tester.startGesture(
      const Offset(10, 10),
      pointer: nextPointer++,
    );
    assert(recognizer.swipeState is ThreeFingerStateIdle);

    await g1.up();
    await g2.up();
    await g3.up();
    await g4.up();
  });
}

class TestDetector extends StatefulWidget {
  const TestDetector({
    required GlobalKey key,
    required this.tracker,
    this.scrollSlop = 0,
    this.pinchSlop = 0,
    this.pitchSlop = 0,
    this.pitchTolerance = 180,
    this.rotateSlop = 0,
    this.decoy = false,
    this.testRecognizer,
  }) : super(key: key);

  final CallbackTracker tracker;

  final bool decoy;
  final TestDoubleFingerGestureRecognizer Function()? testRecognizer;

  final double scrollSlop;
  final double pinchSlop;
  final double pitchSlop;
  final double pitchTolerance;
  final double rotateSlop;

  @override
  State<TestDetector> createState() => _TestDetectorState();
}

class _TestDetectorState extends State<TestDetector> {
  DecoyGestureRecognizer? decoy;
  TestDoubleFingerGestureRecognizer? test;
  ScrollGestureRecognizer? scroll;
  PinchGestureRecognizer? pinch;
  RotateGestureRecognizer? rotate;
  PitchGestureRecognizer? pitch;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 100,
      height: 100,
      child: RawGestureDetector(
        gestures: <Type, GestureRecognizerFactory>{
          if (widget.decoy)
            DecoyGestureRecognizer:
                GestureRecognizerFactoryWithHandlers<DecoyGestureRecognizer>(
              () => decoy = DecoyGestureRecognizer(),
              (DecoyGestureRecognizer instance) {},
            ),
          if (widget.testRecognizer != null)
            TestDoubleFingerGestureRecognizer:
                GestureRecognizerFactoryWithHandlers<
                    TestDoubleFingerGestureRecognizer>(
              () => test = widget.testRecognizer!(),
              (TestDoubleFingerGestureRecognizer instance) {},
            ),
          if (widget.tracker.scroll != null || widget.tracker.scrollEnd != null)
            ScrollGestureRecognizer:
                GestureRecognizerFactoryWithHandlers<ScrollGestureRecognizer>(
              () => scroll = ScrollGestureRecognizer(slop: widget.scrollSlop),
              (ScrollGestureRecognizer instance) {
                instance
                  ..onScroll = widget.tracker.scroll
                  ..onScrollEnd = widget.tracker.scrollEnd;
              },
            ),
          if (widget.tracker.pinch != null)
            PinchGestureRecognizer:
                GestureRecognizerFactoryWithHandlers<PinchGestureRecognizer>(
              () => pinch = PinchGestureRecognizer(slop: widget.pinchSlop),
              (PinchGestureRecognizer instance) {
                instance.onPinch = widget.tracker.pinch;
              },
            ),
          if (widget.tracker.rotate != null)
            RotateGestureRecognizer:
                GestureRecognizerFactoryWithHandlers<RotateGestureRecognizer>(
              () => rotate = RotateGestureRecognizer(slop: widget.rotateSlop),
              (RotateGestureRecognizer instance) {
                instance.onRotate = widget.tracker.rotate;
              },
            ),
          if (widget.tracker.pitch != null)
            PitchGestureRecognizer:
                GestureRecognizerFactoryWithHandlers<PitchGestureRecognizer>(
              () => pitch = PitchGestureRecognizer(
                slop: widget.pitchSlop,
                tolerance: widget.pitchTolerance,
              ),
              (PitchGestureRecognizer instance) {
                instance.onPitch = widget.tracker.pitch;
              },
            ),
        },
      ),
    );
  }
}

//Purpose for this class is to allow us to manually win or lose pointers in the
//gesture arena.
class DecoyGestureRecognizer extends DoubleFingerGestureRecognizer {
  @override
  String get debugDescription => 'decoy';

  @override
  void onUpdate(PointerState previous, PointerState curr) {}

  void win() => pointerState.win();
  void lose() => pointerState.lose();
}

class TestDoubleFingerGestureRecognizer extends DoubleFingerGestureRecognizer {
  TestDoubleFingerGestureRecognizer.loseMoreThanTwo()
      : super(loseWhenMoreThanTwoPointers: true);
  TestDoubleFingerGestureRecognizer.allowMoreThanTwo()
      : super(loseWhenMoreThanTwoPointers: false);

  final List<PointerState> states = [];
  final List<PointerCancelEvent> cancels = [];
  final List<PointerUpEvent> ups = [];
  final List<PointerMoveEvent> moves = [];

  void win() => pointerState.win();
  void lose() => pointerState.lose();

  @override
  String get debugDescription => 'test';

  @override
  void onUpdate(PointerState previous, PointerState curr) {
    states.add(curr);
  }

  @override
  void onCancel(PointerCancelEvent event) {
    cancels.add(event);
    super.onCancel(event);
  }

  @override
  void onUp(PointerUpEvent event) {
    ups.add(event);
    super.onUp(event);
  }

  @override
  void onMove(PointerMoveEvent event) {
    moves.add(event);
    super.onMove(event);
  }
}

class ScrollCallback {
  const ScrollCallback(this.delta);
  final Offset delta;
}

class ScrollEndCallback {
  const ScrollEndCallback(this.position, this.velocityEstimate);
  final Offset position;
  final VelocityEstimate? velocityEstimate;
}

class PinchCallback {
  const PinchCallback(this.center, this.scaleDelta);
  final Offset center;
  final double scaleDelta;
}

class RotateCallback {
  const RotateCallback(this.angleDelta);
  final double angleDelta;
}

class PitchCallback {
  const PitchCallback(this.verticalDelta);
  final double verticalDelta;
}

class CallbackTracker {
  CallbackTracker({
    bool scroll = false,
    bool scrollEnd = false,
    bool pinch = false,
    bool pitch = false,
    bool rotate = false,
  }) {
    this.scroll = scroll ? (delta) => scrolls.add(ScrollCallback(delta)) : null;
    this.scrollEnd = scrollEnd
        ? (position, velocityEstimate) =>
            scrollEnds.add(ScrollEndCallback(position, velocityEstimate))
        : null;
    this.pinch = pinch
        ? (center, scaleDelta) => pinches.add(PinchCallback(center, scaleDelta))
        : null;
    this.pitch = pitch
        ? (verticalDelta) => pitches.add(PitchCallback(verticalDelta))
        : null;
    this.rotate =
        rotate ? (angleDelta) => rotates.add(RotateCallback(angleDelta)) : null;
  }

  final List<ScrollCallback> scrolls = [];
  final List<ScrollEndCallback> scrollEnds = [];
  final List<PinchCallback> pinches = [];
  final List<PitchCallback> pitches = [];
  final List<RotateCallback> rotates = [];

  late final GestureScrollCallback? scroll;
  late final GestureScrollEndCallback? scrollEnd;
  late final GesturePinchCallback? pinch;
  late final GesturePitchCallback? pitch;
  late final GestureRotateCallback? rotate;
}

const Offset centerOffset = Offset(50, 50);

extension _GlobalKeyExt on GlobalKey {
  DecoyGestureRecognizer get decoyRecognizer =>
      (currentState! as _TestDetectorState).decoy!;

  TestDoubleFingerGestureRecognizer get testRecognizer =>
      (currentState! as _TestDetectorState).test!;

  PinchGestureRecognizer get pinchRecognizer =>
      (currentState! as _TestDetectorState).pinch!;

  PitchGestureRecognizer get pitchRecognizer =>
      (currentState! as _TestDetectorState).pitch!;

  ScrollGestureRecognizer get scrollRecognizer =>
      (currentState! as _TestDetectorState).scroll!;

  RotateGestureRecognizer get rotateRecognizer =>
      (currentState! as _TestDetectorState).rotate!;
}

PointerSnapshot _generateSnapshot(Offset o1, Offset o2) {
  return const PointerStateIdle()
      .addFinger(
        PointerTracker(
          PointerDownEvent(position: o1),
          null,
          TrackerState.pending,
        ),
      )
      .addFinger(
        PointerTracker(
          PointerDownEvent(position: o2),
          null,
          TrackerState.pending,
        ),
      )
      .snapshot;
}

void _doubleFingerGestureRecognizerTests() {
  testWidgets('registers first and second pointer down',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final p1 = nextPointer++;
    const o1 = centerOffset;
    final g1 = await tester.startGesture(o1, pointer: p1);

    assert(key.testRecognizer.states.length == 1);
    final oneFingerState =
        key.testRecognizer.pointerState as PointerStateOneFinger;
    assert(oneFingerState.first.pointer == p1);
    assert(oneFingerState.first.downEvent.localPosition == o1);
    assert(oneFingerState.first.lastEvent == oneFingerState.first.downEvent);

    const moveDelta = Offset(10, 10);
    await g1.moveBy(moveDelta);
    assert(oneFingerState.first.lastEvent != oneFingerState.first.downEvent);
    assert(oneFingerState.first.lastEvent.localDelta == moveDelta);

    final p2 = nextPointer++;
    final o2 = Offset(centerOffset.dx + 10, centerOffset.dy + 10);
    final g2 = await tester.startGesture(o2, pointer: p2);

    assert(key.testRecognizer.states.length == 2);
    final twoFingerState =
        key.testRecognizer.pointerState as PointerStateTwoFingers;
    assert(twoFingerState.first.pointer == p1);
    assert(twoFingerState.second.pointer == p2);
    assert(twoFingerState.first.downEvent.localPosition == o1);
    assert(twoFingerState.second.downEvent.localPosition == o2);
    assert(twoFingerState.first.lastEvent != twoFingerState.first.downEvent);
    assert(twoFingerState.second.lastEvent == twoFingerState.second.downEvent);

    await g2.moveBy(moveDelta);
    assert(twoFingerState.second.lastEvent.localDelta == moveDelta);
    assert(twoFingerState.startSnapshot.first.localPosition == o1 + moveDelta);
    assert(twoFingerState.startSnapshot.second.localPosition == o2);
  });

  testWidgets('third pointer is ignored by recognizer when configured',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    await tester.startGesture(centerOffset, pointer: nextPointer++);
    await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateTwoFingers);
    final g3 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateTwoFingers);
    await g3.up();
    assert(key.testRecognizer.states.length == 2);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.isEmpty);
  });

  testWidgets('third pointer loses recognizer when configured',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        testRecognizer: TestDoubleFingerGestureRecognizer.loseMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    final g2 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateTwoFingers);
    final g3 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    await g1.up();
    await g2.up();
    await g3.up();
    assert(key.testRecognizer.states.length == 3);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.isEmpty);
  });

  testWidgets('manually losing pointer cancels gesture',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    (key.testRecognizer.pointerState as PointerStateOneFinger).first.lose();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    await g1.up();
    assert(key.testRecognizer.states.length == 2);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.isEmpty);
  });
  testWidgets('externally losing pointer cancels gesture',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateOneFinger);
    key.decoyRecognizer.win();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    await g1.up();
    assert(key.testRecognizer.states.length == 2);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.isEmpty);
  });

  testWidgets('up event on pointer cancels gesture',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );
    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateOneFinger);
    await g1.up();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    assert(key.testRecognizer.states.length == 2);
    assert(key.testRecognizer.ups.length == 1);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.isEmpty);
  });
  testWidgets('cancel event on pointer cancels gesture',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );
    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateOneFinger);
    await g1.cancel();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    assert(key.testRecognizer.states.length == 2);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.length == 1);
    assert(key.testRecognizer.moves.isEmpty);
  });

  testWidgets('manually losing first pointer cancels gesture',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    final g2 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    (key.testRecognizer.pointerState as PointerStateTwoFingers).first.lose();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    await g1.up();
    await g2.up();
    assert(key.testRecognizer.states.length == 3);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.isEmpty);
  });

  testWidgets('externally losing first pointer cancels gesture',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    final g2 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateTwoFingers);
    (key.decoyRecognizer.pointerState as PointerStateTwoFingers).first.win();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    await g1.up();
    await g2.up();
    assert(key.testRecognizer.states.length == 3);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.isEmpty);
  });

  testWidgets('up event on first pointer cancels gesture',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );
    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    final g2 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateTwoFingers);
    await g1.up();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    await g2.up();
    assert(key.testRecognizer.states.length == 3);
    assert(key.testRecognizer.ups.length == 1);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.isEmpty);
  });
  testWidgets('cancel event on first pointer cancels gesture',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );
    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    final g2 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateTwoFingers);
    await g1.cancel();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    await g2.cancel();
    assert(key.testRecognizer.states.length == 3);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.length == 1);
    assert(key.testRecognizer.moves.isEmpty);
  });
  testWidgets('manually losing second pointer cancels gesture',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    final g2 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    (key.testRecognizer.pointerState as PointerStateTwoFingers).second.lose();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    await g1.up();
    await g2.up();
    assert(key.testRecognizer.states.length == 3);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.isEmpty);
  });

  testWidgets('externally losing second pointer cancels gesture',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    final g2 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    (key.testRecognizer.pointerState as PointerStateTwoFingers).second.lose();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    await g1.up();
    await g2.up();
    assert(key.testRecognizer.states.length == 3);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.isEmpty);
  });

  testWidgets('up event on second pointer cancels gesture',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    final g2 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateTwoFingers);
    await g2.up();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    await g1.up();
    assert(key.testRecognizer.states.length == 3);
    assert(key.testRecognizer.ups.length == 1);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.isEmpty);
  });
  testWidgets('cancel event on second pointer cancels gesture',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    final g2 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateTwoFingers);
    await g2.cancel();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    await g1.cancel();
    assert(key.testRecognizer.states.length == 3);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.length == 1);
    assert(key.testRecognizer.moves.isEmpty);
  });

  testWidgets('move event called', (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateOneFinger);
    await g1.moveBy(const Offset(10, 10));
    assert(key.testRecognizer.pointerState is PointerStateOneFinger);
    assert(key.testRecognizer.states.length == 1);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.length == 1);
  });

  testWidgets('move event called for first pointer',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateTwoFingers);
    await g1.moveBy(const Offset(10, 10));
    assert(key.testRecognizer.pointerState is PointerStateTwoFingers);
    assert(key.testRecognizer.states.length == 2);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.length == 1);
  });

  testWidgets('move event called for second pointer',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    await tester.startGesture(centerOffset, pointer: nextPointer++);
    final g2 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateTwoFingers);
    await g2.moveBy(const Offset(10, 10));
    assert(key.testRecognizer.pointerState is PointerStateTwoFingers);
    assert(key.testRecognizer.states.length == 2);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.length == 1);
  });

  testWidgets('gesture does not restart until all pointers are removed',
      (WidgetTester tester) async {
    final key = GlobalKey();
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        testRecognizer: TestDoubleFingerGestureRecognizer.allowMoreThanTwo,
        tracker: CallbackTracker(),
      ),
    );

    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateOneFinger);
    key.testRecognizer.pointerState.lose();
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    final g2 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    final g3 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    await g1.moveBy(const Offset(10, 10));
    assert(key.testRecognizer.pointerState is PointerStateIdle);
    await g1.up();
    await g2.cancel();
    await g3.up();
    assert(key.testRecognizer.pointerState is PointerStateIdle);

    await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.testRecognizer.pointerState is PointerStateOneFinger);
    assert(key.testRecognizer.states.length == 3);
    assert(key.testRecognizer.ups.isEmpty);
    assert(key.testRecognizer.cancels.isEmpty);
    assert(key.testRecognizer.moves.isEmpty);
  });

  test('snapshot distance calculation', () async {
    final snapshot =
        _generateSnapshot(const Offset(50, -50), const Offset(100, -100));
    final distance = sqrt(pow(50, 2) + pow(-50, 2));
    assert(snapshot.distance == distance);
  });
  test('snapshot center calculation', () async {
    final snapshot =
        _generateSnapshot(const Offset(50, -50), const Offset(100, -100));
    assert(snapshot.center == const Offset(75, -75));
  });
  test('snapshot angle calculation', () async {
    var snapshot = _generateSnapshot(const Offset(1, 0), const Offset(2, 0));
    assert(snapshot.angle == 0);

    snapshot = _generateSnapshot(const Offset(0, 1), const Offset(0, 2));
    assert(snapshot.angle == pi * 0.5);

    snapshot = _generateSnapshot(const Offset(-1, 0), const Offset(-2, 0));
    assert(snapshot.angle == pi);

    snapshot = _generateSnapshot(const Offset(0, -1), const Offset(0, -2));
    assert(snapshot.angle == -pi * 0.5);
  });
}

void _pinchGestureRecognizerTests() {
  testWidgets('normal pinch and up', (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(pinch: true);
    await tester
        .pumpWidget(TestDetector(key: key, decoy: true, tracker: tracker));

    final p1 = centerOffset + const Offset(-10, 0);
    final p2 = centerOffset + const Offset(10, 0);

    //Tap Down
    final g1 = await tester.startGesture(p1, pointer: nextPointer++);
    assert(key.pinchRecognizer.pinchState is PinchStateIdle);

    final g2 = await tester.startGesture(p2, pointer: nextPointer++);
    assert(key.pinchRecognizer.pinchState is PinchStateActive);
    assert(tracker.pinches.isEmpty);

    //Move
    await g1.moveBy(const Offset(-20, 0));
    assert(key.pinchRecognizer.pinchState is PinchStateActive);

    assert(tracker.pinches.length == 1);
    assert(tracker.pinches.last.center == centerOffset + const Offset(-10, 0));
    assert(tracker.pinches.last.scaleDelta == 1);

    //Move
    await g2.moveBy(const Offset(40, 0));
    assert(key.pinchRecognizer.pinchState is PinchStateActive);

    assert(tracker.pinches.length == 2);
    assert(tracker.pinches.last.center == centerOffset + const Offset(10, 0));
    assert(tracker.pinches.last.scaleDelta == 1);

    //Up
    await g1.up();
    assert(key.pinchRecognizer.pinchState is PinchStateIdle);
    assert(tracker.pinches.length == 2);
  });

  testWidgets('pinch slop', (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(pinch: true);
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        tracker: tracker,
        pinchSlop: 10,
      ),
    );

    final p1 = centerOffset + const Offset(-10, 0);
    final p2 = centerOffset + const Offset(10, 0);

    //Tap Down
    final g1 = await tester.startGesture(p1, pointer: nextPointer++);
    assert(key.pinchRecognizer.pinchState is PinchStateIdle);

    final g2 = await tester.startGesture(p2, pointer: nextPointer++);
    assert(key.pinchRecognizer.pinchState is PinchStateWaitingForSlop);
    assert(tracker.pinches.isEmpty);

    //Move
    await g1.moveBy(const Offset(-5, 0));
    assert(key.pinchRecognizer.pinchState is PinchStateWaitingForSlop);
    assert(tracker.pinches.isEmpty);

    //Move
    await g2.moveBy(const Offset(5, 0));
    assert(key.pinchRecognizer.pinchState is PinchStateActive);
    assert(tracker.pinches.isEmpty);
  });
}

void _rotateGestureRecognizerTests() {
  testWidgets('normal rotate and up', (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(rotate: true);
    await tester
        .pumpWidget(TestDetector(key: key, decoy: true, tracker: tracker));

    final p1 = centerOffset + const Offset(-10, 0);
    final p2 = centerOffset + const Offset(10, 0);

    //Tap Down
    final g1 = await tester.startGesture(p1, pointer: nextPointer++);
    assert(key.rotateRecognizer.rotateState is RotateStateIdle);

    final g2 = await tester.startGesture(p2, pointer: nextPointer++);
    assert(key.rotateRecognizer.rotateState is RotateStateActive);
    assert(tracker.rotates.isEmpty);

    //Move
    await g1.moveTo(p2 + const Offset(0, -10));
    assert(key.rotateRecognizer.rotateState is RotateStateActive);

    assert(tracker.rotates.length == 1);
    assert(tracker.rotates.last.angleDelta == 90);

    //Move
    await g2.moveTo(p2 + const Offset(0, -10) + const Offset(5, 5));
    assert(key.rotateRecognizer.rotateState is RotateStateActive);

    assert(tracker.rotates.length == 2);
    assert(tracker.rotates.last.angleDelta == -45);

    //Up
    await g1.up();
    assert(key.rotateRecognizer.rotateState is RotateStateIdle);
    assert(tracker.rotates.length == 2);
  });

  testWidgets('rotate slop', (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(rotate: true);
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        tracker: tracker,
        rotateSlop: 90,
      ),
    );

    final p1 = centerOffset + const Offset(-10, 0);
    final p2 = centerOffset + const Offset(10, 0);

    //Tap Down
    final g1 = await tester.startGesture(p1, pointer: nextPointer++);
    assert(key.rotateRecognizer.rotateState is RotateStateIdle);

    await tester.startGesture(p2, pointer: nextPointer++);
    assert(key.rotateRecognizer.rotateState is RotateStateWaitingForSlop);
    assert(tracker.pinches.isEmpty);

    //Move
    await g1.moveTo(p2 + const Offset(-5, 5));
    assert(key.rotateRecognizer.rotateState is RotateStateWaitingForSlop);
    assert(tracker.pinches.isEmpty);

    //Move
    await g1.moveTo(p2 + const Offset(0, 5));
    assert(key.rotateRecognizer.rotateState is RotateStateActive);
    assert(tracker.pinches.isEmpty);
  });
}

void _pitchGestureRecognizerTests() {
  testWidgets('normal pitch and up', (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(pitch: true);
    await tester
        .pumpWidget(TestDetector(key: key, decoy: true, tracker: tracker));

    const p1 = centerOffset;
    final p2 = centerOffset + const Offset(10, 0);

    //Tap Down
    final g1 = await tester.startGesture(p1, pointer: nextPointer++);
    assert(key.pitchRecognizer.pitchState is PitchStateIdle);

    final g2 = await tester.startGesture(p2, pointer: nextPointer++);
    assert(key.pitchRecognizer.pitchState is PitchStateActive);
    assert(tracker.pitches.isEmpty);

    //Move
    await g1.moveBy(const Offset(0, -2));
    assert(key.pitchRecognizer.pitchState is PitchStateActive);

    assert(tracker.pitches.length == 1);
    assert(tracker.pitches.last.verticalDelta == -1);

    //Move
    await g2.moveBy(const Offset(0, 6));
    assert(key.pitchRecognizer.pitchState is PitchStateActive);

    assert(tracker.pitches.length == 2);
    assert(tracker.pitches.last.verticalDelta == 3);

    //Up
    await g1.up();
    assert(key.pitchRecognizer.pitchState is PitchStateIdle);
    assert(tracker.pitches.length == 2);
  });

  testWidgets('pitch exceed tolerance', (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(pitch: true);
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        tracker: tracker,
        pitchTolerance: 45,
      ),
    );

    const p1 = centerOffset;
    final p2 = centerOffset + const Offset(10, 0);

    //Tap Down
    await tester.startGesture(p1, pointer: nextPointer++);
    assert(key.pitchRecognizer.pitchState is PitchStateIdle);

    final g2 = await tester.startGesture(p2, pointer: nextPointer++);
    assert(key.pitchRecognizer.pitchState is PitchStateActive);
    assert(tracker.pitches.isEmpty);

    //Move
    await g2.moveBy(const Offset(0, 20));
    assert(key.pitchRecognizer.pitchState is PitchStateIdle);
    assert(tracker.pitches.isEmpty);
  });

  testWidgets('pitch starts outside tolerance', (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(pitch: true);
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        tracker: tracker,
        pitchTolerance: 45,
      ),
    );

    const p1 = centerOffset;
    final p2 = centerOffset + const Offset(10, 20);

    //Tap Down
    await tester.startGesture(p1, pointer: nextPointer++);
    assert(key.pitchRecognizer.pitchState is PitchStateIdle);

    await tester.startGesture(p2, pointer: nextPointer++);
    assert(key.pitchRecognizer.pitchState is PitchStateIdle);
    assert(tracker.pitches.isEmpty);
  });

  testWidgets('pitch slop', (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(pitch: true);
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        tracker: tracker,
        pitchSlop: 10,
      ),
    );

    const p1 = centerOffset;
    final p2 = centerOffset + const Offset(10, 0);

    //Tap Down
    final g1 = await tester.startGesture(p1, pointer: nextPointer++);
    assert(key.pitchRecognizer.pitchState is PitchStateIdle);

    final g2 = await tester.startGesture(p2, pointer: nextPointer++);
    assert(key.pitchRecognizer.pitchState is PitchStateWaitingForSlop);
    assert(tracker.pitches.isEmpty);

    //Move
    await g1.moveBy(const Offset(0, -10));
    assert(key.pitchRecognizer.pitchState is PitchStateWaitingForSlop);
    assert(tracker.pitches.isEmpty);

    //Move
    await g2.moveBy(const Offset(0, -10));
    assert(key.pitchRecognizer.pitchState is PitchStateActive);
    assert(tracker.pitches.isEmpty);
  });
}

void _scrollGestureRecognizerTests() {
  testWidgets('normal scroll and up (one pointer)',
      (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(scroll: true, scrollEnd: true);
    await tester
        .pumpWidget(TestDetector(key: key, decoy: true, tracker: tracker));

    //Tap Down
    assert(key.scrollRecognizer.scrollState is ScrollStateIdle);
    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.scrollRecognizer.scrollState is ScrollStateSingle);
    assert(tracker.scrolls.isEmpty);
    assert(tracker.scrollEnds.isEmpty);

    //Move
    await g1.moveBy(const Offset(10, -10));
    assert(key.scrollRecognizer.scrollState is ScrollStateSingle);
    assert(tracker.scrolls.length == 1);
    assert(tracker.scrolls.last.delta == const Offset(10, -10));
    assert(tracker.scrollEnds.isEmpty);

    //Up
    await g1.up();
    assert(key.scrollRecognizer.scrollState is ScrollStateIdle);
    assert(tracker.scrolls.length == 1);
    assert(tracker.scrollEnds.length == 1);
    assert(
      tracker.scrollEnds.last.position == centerOffset + const Offset(10, -10),
    );
  });

  testWidgets('mouse pointer drives scroll (device-kind agnostic)',
      (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(scroll: true, scrollEnd: true);
    await tester
        .pumpWidget(TestDetector(key: key, decoy: true, tracker: tracker));

    // A mouse drag was previously rejected by the touch-only filter; it now
    // activates the recognizer like any other pointer kind.
    assert(key.scrollRecognizer.scrollState is ScrollStateIdle);
    final g1 = await tester.startGesture(
      centerOffset,
      pointer: nextPointer++,
      kind: PointerDeviceKind.mouse,
    );
    assert(key.scrollRecognizer.scrollState is ScrollStateSingle);

    await g1.moveBy(const Offset(10, -10));
    assert(tracker.scrolls.length == 1);
    assert(tracker.scrolls.last.delta == const Offset(10, -10));

    await g1.up();
    assert(key.scrollRecognizer.scrollState is ScrollStateIdle);
    assert(tracker.scrollEnds.length == 1);
  });

  testWidgets('cancel scroll', (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(scroll: true, scrollEnd: true);
    await tester
        .pumpWidget(TestDetector(key: key, decoy: true, tracker: tracker));

    //Tap Down
    assert(key.scrollRecognizer.scrollState is ScrollStateIdle);
    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.scrollRecognizer.scrollState is ScrollStateSingle);
    assert(tracker.scrolls.isEmpty);
    assert(tracker.scrollEnds.isEmpty);

    //Up
    await g1.cancel();
    assert(key.scrollRecognizer.scrollState is ScrollStateIdle);
    assert(tracker.scrolls.isEmpty);
    assert(tracker.scrollEnds.isEmpty);
  });

  testWidgets('normal scroll and up (two pointers)',
      (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(scroll: true, scrollEnd: true);
    await tester
        .pumpWidget(TestDetector(key: key, decoy: true, tracker: tracker));

    final p1 = centerOffset + const Offset(-10, 0);
    final p2 = centerOffset + const Offset(10, 0);

    //Tap Down
    assert(key.scrollRecognizer.scrollState is ScrollStateIdle);
    final g1 = await tester.startGesture(p1, pointer: nextPointer++);
    assert(key.scrollRecognizer.scrollState is ScrollStateSingle);

    final g2 = await tester.startGesture(p2, pointer: nextPointer++);
    assert(key.scrollRecognizer.scrollState is ScrollStateDouble);

    //Move
    await g1.moveBy(const Offset(-20, 0));
    assert(key.scrollRecognizer.scrollState is ScrollStateDouble);
    assert(tracker.scrolls.length == 1);
    assert(tracker.scrolls.last.delta == const Offset(-10, 0));
    assert(tracker.scrollEnds.isEmpty);

    //Move
    await g2.moveBy(const Offset(40, 0));
    assert(key.scrollRecognizer.scrollState is ScrollStateDouble);
    assert(tracker.scrolls.length == 2);
    assert(tracker.scrolls.last.delta == const Offset(20, 0));
    assert(tracker.scrollEnds.isEmpty);

    //Up
    await g1.up();
    assert(key.scrollRecognizer.scrollState is ScrollStateIdle);
    assert(tracker.scrolls.length == 2);
    assert(tracker.scrollEnds.length == 1);
    assert(
      tracker.scrollEnds.last.position == centerOffset + const Offset(10, 0),
    );
  });

  testWidgets('scroll slop (one pointer)', (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(scroll: true, scrollEnd: true);
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        tracker: tracker,
        scrollSlop: 10,
      ),
    );

    //Tap Down
    assert(key.scrollRecognizer.scrollState is ScrollStateIdle);
    final g1 = await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.scrollRecognizer.scrollState is ScrollStateSingleWaitingForSlop);

    //Move
    await g1.moveBy(const Offset(0, 5));
    assert(key.scrollRecognizer.scrollState is ScrollStateSingleWaitingForSlop);

    //Move
    await g1.moveBy(const Offset(0, 5));
    assert(key.scrollRecognizer.scrollState is ScrollStateSingle);

    //Tap Down
    await tester.startGesture(centerOffset, pointer: nextPointer++);
    assert(key.scrollRecognizer.scrollState is ScrollStateDouble);
    assert(tracker.scrolls.isEmpty);
    assert(tracker.scrollEnds.isEmpty);
  });

  testWidgets('scroll slop (two pointers)', (WidgetTester tester) async {
    final key = GlobalKey();
    final tracker = CallbackTracker(scroll: true, scrollEnd: true);
    await tester.pumpWidget(
      TestDetector(
        key: key,
        decoy: true,
        tracker: tracker,
        scrollSlop: 10,
      ),
    );

    final p1 = centerOffset + const Offset(-10, 0);
    final p2 = centerOffset + const Offset(10, 0);

    //Tap Down
    assert(key.scrollRecognizer.scrollState is ScrollStateIdle);
    final g1 = await tester.startGesture(p1, pointer: nextPointer++);
    assert(key.scrollRecognizer.scrollState is ScrollStateSingleWaitingForSlop);

    final g2 = await tester.startGesture(p2, pointer: nextPointer++);
    assert(key.scrollRecognizer.scrollState is ScrollStateDoubleWaitingForSlop);

    //Move
    await g1.moveBy(const Offset(-10, 0));
    assert(key.scrollRecognizer.scrollState is ScrollStateDoubleWaitingForSlop);

    //Move
    await g2.moveBy(const Offset(-10, 0));
    assert(key.scrollRecognizer.scrollState is ScrollStateDouble);
    assert(tracker.scrolls.isEmpty);
    assert(tracker.scrollEnds.isEmpty);
  });
}
