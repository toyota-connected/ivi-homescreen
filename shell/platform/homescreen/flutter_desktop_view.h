#pragma once

class FlutterView;

// Opaque reference for the FlutterView. This is separate from the
// controller so that it can be provided to plugins without giving them access
// to all the controller-based functionality.
struct FlutterDesktopView {
  // The View that (indirectly) owns this state object.
  FlutterView* view{};
};
