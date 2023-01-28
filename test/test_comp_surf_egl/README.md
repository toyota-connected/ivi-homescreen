# Test - Compositor Surface EGL

### C++
```
mkdir cmake-build-debug 
cd cmake-build-debug
CC=/usr/lib/llvm-12/bin/clang CXX=/usr/lib/llvm-12/bin/clang++ cmake ..
sudo make install
```

### ivi-homescreen

Ensure ivi-homescreen is built with `-DBUILD_PLUGIN_COMP_SURF=ON`

### flutter

```
flutter run -d flutter-auto
q
homescreen --b=/tmp/test_comp_surf_egl
```
