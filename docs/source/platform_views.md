# Hosting native views in your Flutter app with Platform Views
Platform Views allow you to embed native UI components directly into your Flutter app. This is particularly useful for integrating complex native widgets or leveraging platform-specific features that are not available in Flutter.

`ivi-homescreen`'s implementation allows you to execute native C++ code in a plugin to register and manage the lifecycle and interactions of platform views. When created, a platform view is backed by a compositor surface (corresponding to a Flutter Widget) that can be positioned in the Flutter UI, and a native view that is rendered on top of the Flutter content at the corresponding position.

The plugin can then manage the native view's lifecycle and interactions, while the Flutter app can communicate with the plugin to control the view and respond to user input - this can be done via Method Channels, Event Channels, FFI, Dart native ports, and other solutions.

## On the Dart side

// TODO: add example of how to define a platform view via `AndroidView` class

In Dart, create a `Widget` (eg. `ExampleViewWidget`) as follows:

```dart
// necessary imports
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

// your widget definition
class ExampleViewWidget extends StatelessWidget {
    ...

    @override
    Widget build(BuildContext context) {
        // This is used in the platform side to register the view
        const String viewType = 'your-view-type-name';

        // Pass parameters to the platform side
        const Map<String, dynamic> creationParams = <String, dynamic>{
            'param1': 'value1',
            'param2': 'value2',
        };

        return AndroidView(
            key: ValueKey(viewType), // Use a unique key if you have multiple instances of the same view type AND/OR want to avoid unnecessary native teardown/recreation
            viewType: viewType,
            creationParams: creationParams,
            creationParamsCodec: const StandardMessageCodec(),
            onPlatformViewCreated: (int id) {
                // Optionally handle the platform view creation callback
            },
            hitTestBehavior: PlatformViewHitTestBehavior.opaque, // or .transparent based on your needs
            gestureRecognizers: const {}, // or, define gesture recognizers if you want to handle gestures on the platform view
        );
```



## On the native side

See [Plugin ABI documentation](plugins.md#plugin-abi) for details on how to define the plugin ABI and build your plugin.

### Platform View registration

Inside your `FlutterPluginRegister` hook function, you can register a platform view by calling the `RegisterPlatformView` method on the `platform_views_handler` of the plugin registrar's engine. This method takes a unique view type name and a callback function that will be called to create instances of the platform view when requested by the Flutter app.

```cpp
void ExamplePlatformViewPluginRegister(
    FlutterDesktopPluginRegistrarRef registrar) {
  // Method channel setup
  ...

  // Platform view setup
  registrar->engine->platform_views_handler->RegisterPlatformView(
      "your-view-type-name",
      plugin_example_platform_view::ExamplePlatformViewPlugin::RegisterWithRegistrar
  );
}

// dynamic plugin support
void FlutterPluginRegister(
    FlutterDesktopPluginRegistrarRef registrar) {
  ExamplePlatformViewPluginRegister(registrar);
}
```
