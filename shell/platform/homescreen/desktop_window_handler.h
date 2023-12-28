#ifndef FLUTTER_SHELL_PLATFORM_HOMESCREEN_WINDOW_HANDLER_H
#define FLUTTER_SHELL_PLATFORM_HOMESCREEN_WINDOW_HANDLER_H

#include <memory>

#include "flutter/shell/platform/common/client_wrapper/include/flutter/binary_messenger.h"
#include "flutter/shell/platform/common/client_wrapper/include/flutter/method_channel.h"

class FlutterView;

namespace flutter {

// Implements min-sizes for desktop apps.
//
class DesktopWindowHandler final {
 public:
  explicit DesktopWindowHandler(flutter::BinaryMessenger* messenger,
                                FlutterView* view);
};

}  // namespace flutter

#endif  // FLUTTER_SHELL_PLATFORM_HOMESCREEN_WINDOW_HANDLER_H
