# Firebase Storage Plugin

* Pigeon interface

* requires firebase cpp sdk: https://github.com/firebase/firebase-cpp-sdk to exist and be built prior.

* This plugin is included when the following variable is set:

```
  -DBUILD_PLUGIN_FIREBASE_STORAGE=ON
```

## Building Firebase C++ SDK

    pip3 install absl-py

GCC

    git clone https://github.com/firebase/firebase-cpp-sdk.git
    cd firebase-cpp-sdk
    mkdir build && cd build
    cmake .. -DFIREBASE_USE_BORINGSSL=YES -DFIREBASE_LINUX_USE_CXX11_ABI=ON
    make -j

Clang

    git clone https://github.com/firebase/firebase-cpp-sdk.git
    cd firebase-cpp-sdk
    mkdir build && cd build
    CC=/usr/bin/clang CXX=/usr/bin/clang++ cmake .. -DFIREBASE_USE_BORINGSSL=YES -DFIREBASE_LINUX_USE_CXX11_ABI=ON -DCMAKE_CXX_FLAGS="-stdlib=libc++ -Wno-deprecated-builtins"
    make -j

## Functional Test Case

    git clone https://github.com/firebase/flutterfire.git
    flutterfire/packages/firebase_storage/firebase_storage/example

Change options to default to Android when Linux

```
diff --git a/packages/firebase_storage/firebase_storage/example/lib/firebase_options.dart b/packages/firebase_storage/firebase_storage/example/lib/firebase_options.dart
index 47d540bb4..450949cca 100644
--- a/packages/firebase_storage/firebase_storage/example/lib/firebase_options.dart
+++ b/packages/firebase_storage/firebase_storage/example/lib/firebase_options.dart
@@ -25,6 +25,7 @@ class DefaultFirebaseOptions {
     }
     // ignore: missing_enum_constant_in_switch
     switch (defaultTargetPlatform) {
+      case TargetPlatform.linux:
       case TargetPlatform.android:
         return android;
       case TargetPlatform.iOS:
```

Run flutter application:

    flutter run -d desktop-homescreen

Monitor terminal output for interaction with application

## Reference Firebase C++ SDK Build

    -DFIREBASE_CPP_SDK_DIR=<path to firebase-sdk-sdk root folder>
    -DFIREBASE_SDK_LIBDIR=<path to firebase-sdk-sdk build root folder>