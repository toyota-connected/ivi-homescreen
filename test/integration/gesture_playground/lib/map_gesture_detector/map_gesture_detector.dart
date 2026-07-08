import 'package:collection/collection.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/widgets.dart';

import '_pinch_gesture_recognizer.dart';
import '_pitch_gesture_recognizer.dart';
import '_rotate_gesture_detector.dart';
import '_scroll_gesture_recognizer.dart';
import '_three_finger_swipe_gesture_recognizer.dart';

/// Custom gesture detector that combines the tap, long press, scroll, pinch,
/// rotate, pitch, and three-finger swipe gestures.
class MapGestureDetector extends StatelessWidget {
  ///Constructor
  const MapGestureDetector({
    super.key,
    this.onTapDown,
    this.onTap,
    this.onLongPress,
    this.onScroll,
    this.onScrollEnd,
    this.onPinch,
    this.onPitch,
    this.onRotate,
    this.onThreeFingerSwipe,
    this.onThreeFingerSwipeEnd,
    this.scrollSlop,
    this.pinchSlop,
    this.pitchSlop,
    this.threeFingerSwipeSlop,
    this.pitchTolerance = 30,
    this.rotateSlop = 15,
    this.behavior,
    this.supportedDevices,
    this.child,
  });

  ///
  final GestureTapDownCallback? onTapDown;

  ///
  final GestureTapCallback? onTap;

  ///
  final GestureLongPressCallback? onLongPress;

  ///
  final GestureScrollCallback? onScroll;

  ///
  final GestureScrollEndCallback? onScrollEnd;

  ///
  final GesturePinchCallback? onPinch;

  ///
  final GesturePitchCallback? onPitch;

  ///
  final GestureRotateCallback? onRotate;

  ///Called with the centroid delta of a three-finger swipe.
  final GestureThreeFingerSwipeCallback? onThreeFingerSwipe;

  ///Called when a three-finger swipe ends, with position and velocity.
  final GestureThreeFingerSwipeEndCallback? onThreeFingerSwipeEnd;

  ///Behavior is passed on to underlying `RawGestureDetector`
  final HitTestBehavior? behavior;

  ///Restricts which pointer device kinds drive the gestures. Null (the
  ///default) allows every kind: touch, mouse, trackpad, and stylus.
  final Set<PointerDeviceKind>? supportedDevices;

  ///
  final Widget? child;

  ///Distance that a must first be exceeded relative to the initial tap down
  ///event of the scroll gesture in order to start sending callbacks.
  final double? scrollSlop;

  ///Difference in distance between the two pointers that a must first be
  ///exceeded (relative to their initial positions) in order to start
  ///sending callbacks related to the pinch gesture.
  final double? pinchSlop;

  ///Difference in distance between the current and starting center point of
  ///the two pointers that must first be exceeded in order to start sending
  ///callbacks related to the pitch gesture.
  final double? pitchSlop;

  ///Distance the centroid of the three pointers must travel before the
  ///three-finger swipe gesture starts sending callbacks.
  final double? threeFingerSwipeSlop;

  ///Angle between the two pointers of a pitch gesture that, if exceeded at
  ///any point, the gesture is terminated for those pointers.
  final double pitchTolerance;

  ///Angle difference (in degrees) between the current and starting lines
  ///between the two pointers that must first be exceeded in order to start
  ///sending callbacks related to the rotate gesture.
  final double rotateSlop;

  double? get _defaultTouchSlop => WidgetsBinding.instance.platformDispatcher
      .views.firstOrNull?.gestureSettings.physicalTouchSlop;

  @override
  Widget build(BuildContext context) {
    double? defaultTouchSlop;
    final double scrollSlop;
    final double pitchSlop;
    final double pinchSlop;

    if (this.scrollSlop != null) {
      scrollSlop = this.scrollSlop!;
    } else {
      final deviceTouchSlop = defaultTouchSlop ??= _defaultTouchSlop;
      scrollSlop = deviceTouchSlop != null ? deviceTouchSlop * 2 : kPanSlop;
    }

    pinchSlop = this.pinchSlop ??
        (defaultTouchSlop ??= _defaultTouchSlop) ??
        kScaleSlop;

    if (this.pitchSlop != null) {
      pitchSlop = this.pitchSlop!;
    } else {
      final deviceTouchSlop = defaultTouchSlop ??= _defaultTouchSlop;
      pitchSlop = deviceTouchSlop != null ? deviceTouchSlop * 2 : kPanSlop;
    }

    final threeFingerSwipeSlop = this.threeFingerSwipeSlop ??
        (defaultTouchSlop ??= _defaultTouchSlop) ??
        kPanSlop;

    return RawGestureDetector(
      behavior: behavior,
      gestures: <Type, GestureRecognizerFactory>{
        if (onTap != null || onTapDown != null)
          TapGestureRecognizer:
              GestureRecognizerFactoryWithHandlers<TapGestureRecognizer>(
            TapGestureRecognizer.new,
            (TapGestureRecognizer instance) {
              instance
                ..onTapDown = onTapDown
                ..onTap = onTap;
            },
          ),
        if (onLongPress != null)
          LongPressGestureRecognizer:
              GestureRecognizerFactoryWithHandlers<LongPressGestureRecognizer>(
            LongPressGestureRecognizer.new,
            (LongPressGestureRecognizer instance) {
              instance.onLongPress = onLongPress;
            },
          ),
        if (onScroll != null || onScrollEnd != null)
          ScrollGestureRecognizer:
              GestureRecognizerFactoryWithHandlers<ScrollGestureRecognizer>(
            () => ScrollGestureRecognizer(
              slop: scrollSlop,
              claimVictoryOnStart: false,
              supportedDevices: supportedDevices,
            ),
            (ScrollGestureRecognizer instance) {
              instance
                ..onScroll = onScroll
                ..onScrollEnd = onScrollEnd;
            },
          ),
        if (onPinch != null)
          PinchGestureRecognizer:
              GestureRecognizerFactoryWithHandlers<PinchGestureRecognizer>(
            () => PinchGestureRecognizer(
              slop: pinchSlop,
              claimVictoryOnStart: false,
              supportedDevices: supportedDevices,
            ),
            (PinchGestureRecognizer instance) {
              instance.onPinch = onPinch;
            },
          ),
        if (onRotate != null)
          RotateGestureRecognizer:
              GestureRecognizerFactoryWithHandlers<RotateGestureRecognizer>(
            () => RotateGestureRecognizer(
              slop: rotateSlop,
              claimVictoryOnStart: false,
              supportedDevices: supportedDevices,
            ),
            (RotateGestureRecognizer instance) {
              instance.onRotate = onRotate;
            },
          ),
        if (onPitch != null)
          PitchGestureRecognizer:
              GestureRecognizerFactoryWithHandlers<PitchGestureRecognizer>(
            () => PitchGestureRecognizer(
              slop: pitchSlop,
              tolerance: pitchTolerance,
              claimVictoryOnStart: true,
              supportedDevices: supportedDevices,
            ),
            (PitchGestureRecognizer instance) {
              instance.onPitch = onPitch;
            },
          ),
        if (onThreeFingerSwipe != null || onThreeFingerSwipeEnd != null)
          ThreeFingerSwipeGestureRecognizer:
              GestureRecognizerFactoryWithHandlers<
                  ThreeFingerSwipeGestureRecognizer>(
            () => ThreeFingerSwipeGestureRecognizer(
              slop: threeFingerSwipeSlop,
              claimVictoryOnStart: false,
              supportedDevices: supportedDevices,
            ),
            (ThreeFingerSwipeGestureRecognizer instance) {
              instance
                ..onSwipe = onThreeFingerSwipe
                ..onSwipeEnd = onThreeFingerSwipeEnd;
            },
          ),
      },
      child: child,
    );
  }

  // coverage:ignore-start
  @override
  void debugFillProperties(DiagnosticPropertiesBuilder properties) {
    super.debugFillProperties(properties);
    properties
      ..add(DiagnosticsProperty('rotateSlop', rotateSlop))
      ..add(DiagnosticsProperty('pitchTolerance', pitchTolerance))
      ..add(DiagnosticsProperty('pitchSlop', pitchSlop))
      ..add(DiagnosticsProperty('pinchSlop', pinchSlop))
      ..add(DiagnosticsProperty('scrollSlop', scrollSlop))
      ..add(DiagnosticsProperty('behavior', behavior))
      ..add(ObjectFlagProperty.has('onRotate', onRotate))
      ..add(ObjectFlagProperty.has('onPitch', onPitch))
      ..add(ObjectFlagProperty.has('onPinch', onPinch))
      ..add(ObjectFlagProperty.has('onScrollEnd', onScrollEnd))
      ..add(ObjectFlagProperty.has('onScroll', onScroll))
      ..add(ObjectFlagProperty.has('onLongPress', onLongPress))
      ..add(ObjectFlagProperty.has('onTap', onTap))
      ..add(ObjectFlagProperty.has('onTapDown', onTapDown));
  }
  // coverage:ignore-end
}
