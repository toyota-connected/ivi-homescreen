
#include "flutter_desktop_messenger.h"

FlutterDesktopMessengerRef FlutterDesktopMessengerAddRef(
    FlutterDesktopMessengerRef messenger) {
  messenger->AddRef();
  return messenger;
}

void FlutterDesktopMessengerRelease(FlutterDesktopMessengerRef messenger) {
  messenger->Release();
}

bool FlutterDesktopMessengerIsAvailable(FlutterDesktopMessengerRef messenger) {
  return messenger->GetEngine() != nullptr;
}

FlutterDesktopMessengerRef FlutterDesktopMessengerLock(
    FlutterDesktopMessengerRef messenger) {
  messenger->GetMutex().lock();
  return messenger;
}

void FlutterDesktopMessengerUnlock(FlutterDesktopMessengerRef messenger) {
  messenger->GetMutex().unlock();
}
