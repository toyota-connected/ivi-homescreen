# Camera Plugin Development

This file contains the development plan for the Camera Plugin project.

## Current Progress

The Camera Plugin can currently handle `availableCameras`, `create`, and `initialize`.

## Future Development

The following aspects are still under development:

- Session and FrameSink classes: These classes need to be created to handle different parts of the camera's operation.

## Build libcamera

### Clang

    git clone https://git.libcamera.org/libcamera/libcamera.git
    cd libcamera
    CC=/usr/bin/clang CXX=/usr/bin/clang++ CXX_FLAGS=-stdlib=libc++ LDFLAGS=-stdlib=libc++ meson build -D lc-compliance=false
    ninja -C build install -j `nproc`

### GCC

    git clone https://git.libcamera.org/libcamera/libcamera.git
    cd libcamera
    meson build
    ninja -C build install -j `nproc`

## libcamera logging output

To set logging to show only errors:

    export LIBCAMERA_LOG_LEVELS=*:ERROR

## Flutter Example

You can refer to this **[Flutter Example](https://github.com/flutter/packages/tree/main/packages/camera/camera/example)
** for more detailed usage of camera plugin.