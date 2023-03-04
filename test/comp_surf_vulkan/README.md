# Test - Compositor Surface Vulkan

### C++
```
mkdir cmake-build-debug 
cd cmake-build-debug
CC=/usr/lib/llvm-12/bin/clang CXX=/usr/lib/llvm-12/bin/clang++ cmake ..
sudo make install
```

### ivi-homescreen

Ensure `ivi-homescreen` is built with the following flag:

    -DBUILD_PLUGIN_COMP_SURF=ON


### flutter

The following expects the use of Flutter Workspace Automation:

```
flutter run -d flutter-auto
q
flutter-auto --b=/tmp/test_comp_surf_vulkan
```
