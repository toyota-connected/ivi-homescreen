# Firebase Auth Plugin

* Pigeon interface

* requires firebase cpp sdk: https://github.com/firebase/firebase-cpp-sdk to exist and be built prior.

* Set the following variable to include plugin

```
  -DBUILD_PLUGIN_FIREBASE_AUTH=ON
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
    flutterfire/packages/firebase_auth/firebase_auth/example

Change options to default to Android when Linux

```diff --git a/packages/firebase_auth/firebase_auth/example/lib/firebase_options.dart b/packages/firebase_auth/firebase_auth/example/lib/firebase_options.dart
index ba1a88dc9..25352ed28 100644
--- a/packages/firebase_auth/firebase_auth/example/lib/firebase_options.dart
+++ b/packages/firebase_auth/firebase_auth/example/lib/firebase_options.dart
@@ -24,6 +24,7 @@ class DefaultFirebaseOptions {
       return web;
     }
     switch (defaultTargetPlatform) {
+      case TargetPlatform.linux:
       case TargetPlatform.android:
         return android;
       case TargetPlatform.iOS:
@@ -32,11 +33,6 @@ class DefaultFirebaseOptions {
         return macos;
       case TargetPlatform.windows:
         return macos;
-      case TargetPlatform.linux:
-        throw UnsupportedError(
-          'DefaultFirebaseOptions have not been configured for linux - '
-          'you can reconfigure this by running the FlutterFire CLI again.',
-        );
       default:
         throw UnsupportedError(
           'DefaultFirebaseOptions are not supported for this platform.',
```

Run flutter application:

    flutter run -d desktop-homescreen

Monitor terminal output for interaction with application

## Reference Firebase C++ SDK Build

    -DFIREBASE_CPP_SDK_DIR=<path to firebase-sdk-sdk root folder>
    -DFIREBASE_SDK_LIBDIR=<path to firebase-sdk-sdk build root folder>