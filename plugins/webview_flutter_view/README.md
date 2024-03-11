# webview_flutter

Chromium Embedded Framework (CEF)

## Generate messages.g.h and messages.g.cc

    flutter pub run pigeon --input pigeons/android_webview.dart --cpp_header_out messages.g.h --cpp_source_out messages.g.cc --cpp_namespace plugin_webview_flutter