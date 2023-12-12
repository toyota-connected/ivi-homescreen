# Firebase Core Plugin

* Pigeon interface

* requires firebase cpp sdk: https://github.com/firebase/firebase-cpp-sdk to exist and be built prior.

* This plugin is included when the following variable is set:

```
  -DBUILD_PLUGIN_CLOUD_FIRESTORE=ON
```

## Building Firebase C++ SDK

    git clone https://github.com/firebase/firebase-cpp-sdk.git
    cd firebase-cpp-sdk
    mkdir build && cd build
    cmake .. -DFIREBASE_USE_BORINGSSL=YES -DFIREBASE_LINUX_USE_CXX11_ABI=ON
    make -j

## Reference Firebase C++ SDK Build

    -DFIREBASE_CPP_SDK_DIR=<path to firebase-sdk-sdk root folder>
    -DFIREBASE_SDK_LIBDIR=<path to firebase-sdk-sdk build root folder>